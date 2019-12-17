// LINT_C_FILE: Do not remove this line. It ensures cpplint treats this as a C file.

#include "src/stirling/bcc_bpf_interface/go_grpc_types.h"

#define HEADER_COUNT 64

BPF_PERF_OUTPUT(go_grpc_header_events);
BPF_PERF_OUTPUT(go_grpc_data_events);

// BPF programs are limited to a 512-byte stack. We store this value per CPU
// and use it as a heap allocated value.
BPF_PERCPU_ARRAY(data_event_buffer_heap, struct go_grpc_data_event_t, 1);
static __inline struct go_grpc_data_event_t* get_data_event() {
  u32 kZero = 0;
  return data_event_buffer_heap.lookup(&kZero);
}

// Key: TGID
// Value: Symbol addresses for the binary with that TGID.
BPF_HASH(symaddrs_map, uint32_t, struct conn_symaddrs_t);

// From golang source:
// //A HeaderField is a name-value pair. Both the name and value are
// // treated as opaque sequences of octets.
// type HeaderField struct {
//   Name, Value string
//
//   // Sensitive means that this header field should never be
//   // indexed.
//   Sensitive bool
// }
struct HPackHeaderField {
  struct gostring name;
  struct gostring value;
  bool sensitive;
};

// From golang source:
// type FD struct {
//   fdmu fdMutex
//   // fdMutex is 16 bytes.
//   type fdMutex struct {
//     state uint64
//     rsema uint32
//     wsema uint32
//   }
//   Sysfd int
//   ...
// }
struct FD {
  uint64_t fdmu0;
  uint64_t fdmu1;
  int64_t sysfd;
};

// C Implementation from FrameHeader struct from golang source code:
// type FrameHeader struct {
//   valid bool  // 1 byte
//   Type FrameType  // 1 byte
//   Flags Flags  // 1 byte, a bit mask
//   Length uint32  // 4 bytes
//   StreamID uint32  // 4 bytes
//}
struct FrameHeader {
  bool valid;
  uint8_t frame_type;
  uint8_t flags;
  uint32_t length;
  uint32_t stream_id;
};

// C Implementation of the DataFrame struct from golang source:
// type DataFrame struct {
//   FrameHeader
//   data []byte
// }
struct DataFrame {
  struct FrameHeader header;
  struct go_byte_array data;
};

// This function accesses one of the following:
//   conn.conn.conn.fd.pfd.Sysfd
//   conn.conn.fd.pfd.Sysfd
//   conn.fd.pfd.Sysfd
// The right one to use depends on the context (e.g. whether the connection uses TLS or not).
//
// (gdb) x ($sp+8)
// 0xc000069e48:  0x000000c0001560e0
// (gdb) x/2gx (0x000000c0001560e0+112)
// 0xc000156150:  0x0000000000b2b1e0  0x000000c0000caf00
// (gdb) x 0x0000000000b2b1e0
// 0xb2b1e0 <go.itab.*google.golang.org/grpc/internal/transport.bufWriter,io.Writer>:
// 0x00000000009c9400 (gdb) x/2gx (0x000000c0000caf00+40) 0xc0000caf28:  0x0000000000b3bf60
// 0x000000c00000ec20 (gdb) x 0x0000000000b3bf60 0xb3bf60
// <go.itab.*google.golang.org/grpc/credentials/internal.syscallConn,net.Conn>: 0x00000000009f66c0
// (gdb) x/2gx 0x000000c00000ec20
// 0xc00000ec20:  0x0000000000b3bea0  0x000000c000059180
// (gdb) x 0x0000000000b3bea0
// 0xb3bea0 <go.itab.*crypto/tls.Conn,net.Conn>:  0x00000000009f66c0
// (gdb) x/2gx 0x000000c000059180
// 0xc000059180:  0x0000000000b3c020  0x000000c000010048
// (gdb) x 0x0000000000b3c020
// 0xb3c020 <go.itab.*net.TCPConn,net.Conn>:  0x00000000009f66c0
static __inline int32_t get_fd_from_conn_intf(struct go_interface conn_intf) {
  uint64_t current_pid_tgid = bpf_get_current_pid_tgid();
  uint32_t tgid = current_pid_tgid >> 32;
  struct conn_symaddrs_t* symaddrs = symaddrs_map.lookup(&tgid);
  if (symaddrs == NULL) {
    return 0;
  }

  if (conn_intf.type == symaddrs->syscall_conn) {
    const int kSyscallConnConnOffset = 0;
    bpf_probe_read(&conn_intf, sizeof(conn_intf), conn_intf.ptr + kSyscallConnConnOffset);
  }

  if (conn_intf.type == symaddrs->tls_conn) {
    const int kTLSConnConnOffset = 0;
    bpf_probe_read(&conn_intf, sizeof(conn_intf), conn_intf.ptr + kTLSConnConnOffset);
  }

  if (conn_intf.type != symaddrs->tcp_conn) {
    return 0;
  }

  void* fd_ptr;
  bpf_probe_read(&fd_ptr, sizeof(fd_ptr), conn_intf.ptr);

  struct FD fd;
  const int kFDOffset = 0;
  bpf_probe_read(&fd, sizeof(fd), fd_ptr + kFDOffset);

  return fd.sysfd;
}

// Returns the file descriptor from a http2.Framer object.
static __inline int32_t get_fd_from_http2_framer(const void* framer_ptr) {
  // From llvm-dwarfdump -n w ./client
  // 0x00070b39: DW_TAG_member
  //              DW_AT_name  ("w")
  //              DW_AT_data_member_location  (112)
  //              DW_AT_type  (0x000000000004a883 "io.Writer")
  //              DW_AT_unknown_2903  (0x00)
  const int kFramerIOWriterOffset = 112;
  struct go_interface io_writer_interface;
  bpf_probe_read(&io_writer_interface, sizeof(io_writer_interface),
                 framer_ptr + kFramerIOWriterOffset);

  const int kIOWriterConnOffset = 40;
  struct go_interface conn_intf = {};
  bpf_probe_read(&conn_intf, sizeof(conn_intf), io_writer_interface.ptr + kIOWriterConnOffset);

  return get_fd_from_conn_intf(conn_intf);
}

static __inline void fill_header_field(struct go_grpc_http2_header_event_t* event,
                                       const struct HPackHeaderField* user_space_ptr) {
  struct HPackHeaderField field;
  bpf_probe_read(&field, sizeof(struct HPackHeaderField), user_space_ptr);

  event->name.size = BPF_LEN_CAP(field.name.len, HEADER_FIELD_STR_SIZE);
  bpf_probe_read(event->name.msg, event->name.size, field.name.ptr);

  event->value.size = BPF_LEN_CAP(field.value.len, HEADER_FIELD_STR_SIZE);
  bpf_probe_read(event->value.msg, event->value.size, field.value.ptr);
}

struct go_grpc_framer_t {
  void* writer;
  void* http2_framer;
};

// Probes (*loopyWriter).writeHeader(uint32, bool, []hpack.HeaderField, func()) inside gRPC-go,
// which writes HTTP2 headers to the server.
int probe_loopy_writer_write_header(struct pt_regs* ctx) {
  // The following code tries to extract the file descriptor held by the loopyWriter object, when
  // probing the (*loopyWriter).writeHeader() function call. The whole structure looks like:
  // type loopyWriter struct {
  //   ...  // 40 bytes offset
  //   framer *framer
  //   type framer struct {
  //     writer *bufWriter
  //     type bufWriter struct {
  //       ...  // 40 bytes offset
  //       conn net.Conn
  //       type net.Conn interface {
  //         ...  // 8 bytes offset
  //         data  // An pointer to *net.TCPConn, which implements the net.Conn interface.
  //         type TCPConn struct {
  //           conn  // conn is embedded inside TCPConn, which is defined as follows.
  //           type conn struct {
  //             fd *netFD
  //             type netFD struct {
  //               pfd poll.FD
  //               type FD struct {
  //                 ...  // 16 bytes offset
  //                 Sysfd int
  //               }
  //             }
  //           }
  //         }
  //       }
  //     }
  //   }
  // }
  const void* sp = (const void*)PT_REGS_SP(ctx);
  const int kLoopyWriterParamOffset = 8;
  const void* loopy_writer_ptr = *(const void**)(sp + kLoopyWriterParamOffset);

  const int kFramerFieldOffset = 40;
  const void* framer_ptr = *(const void**)(loopy_writer_ptr + kFramerFieldOffset);
  struct go_grpc_framer_t go_grpc_framer = {};
  bpf_probe_read(&go_grpc_framer, sizeof(go_grpc_framer), framer_ptr);

  const int32_t fd = get_fd_from_http2_framer(go_grpc_framer.http2_framer);

  const int kStreamIDParamOffset = 16;
  const uint32_t stream_id = *(uint32_t*)(sp + kStreamIDParamOffset);

  uint64_t tgid = bpf_get_current_pid_tgid() >> 32;
  struct conn_info_t* conn_info = get_conn_info(tgid, fd);
  if (conn_info == NULL) {
    return 0;
  }

  struct go_grpc_http2_header_event_t event = {};
  event.attr.type = kGRPCWriteHeader;
  event.attr.htype = kHeaderEventWrite;
  event.attr.timestamp_ns = bpf_ktime_get_ns();
  event.attr.conn_id = conn_info->conn_id;
  event.attr.stream_id = stream_id;

  const int kHeaderFieldSliceParamOffset = 24;
  const struct go_ptr_array fields =
      *(const struct go_ptr_array*)(sp + kHeaderFieldSliceParamOffset);
  const struct HPackHeaderField* fields_ptr = fields.ptr;

  // Using 'int i' below seems prevent loop getting rolled.
  // TODO(yzhao): Investigate and fix.
#pragma unroll
  for (unsigned int i = 0, pos = 0; i < LOOP_LIMIT && i < fields.len; ++i) {
    fill_header_field(&event, fields_ptr + i);
    go_grpc_header_events.perf_submit(ctx, &event, sizeof(event));
  }

  return 0;
}

// Probes (*http2Client).operateHeaders(*http2.MetaHeadersFrame) inside gRPC-go, which processes
// HTTP2 headers of the received responses.
int probe_http2_client_operate_headers(struct pt_regs* ctx) {
  const void* sp = (const void*)PT_REGS_SP(ctx);

  const int kFieldsParamOffset = 16;
  const void* frame_ptr = *(const void**)(sp + kFieldsParamOffset);

  const void* frame_header_ptr = *(const void**)frame_ptr;
  const int kStreamIDOffset = 8;
  const uint32_t stream_id = *(const uint32_t*)(frame_header_ptr + kStreamIDOffset);

  const int kFieldsOffset = 8;
  const struct go_ptr_array fields = *(const struct go_ptr_array*)(frame_ptr + kFieldsOffset);
  const struct HPackHeaderField* fields_ptr = fields.ptr;

  // TODO(yzhao): We saw some arbitrary large slices received by operateHeaders(), it's not clear
  // what conditions result into them.
  if (fields.len > 100 || fields.len <= 0 || fields.cap <= 0) {
    return 0;
  }

  uint64_t tgid = bpf_get_current_pid_tgid() >> 32;

  const int kHTTP2ClientParamOffset = 8;
  const void* http2_client_ptr = *(const void**)(sp + kHTTP2ClientParamOffset);

  struct go_interface conn_intf = {};
  const int kHTTP2ClientConnFieldOffset = 64;
  bpf_probe_read(&conn_intf, sizeof(conn_intf), http2_client_ptr + kHTTP2ClientConnFieldOffset);

  const uint32_t fd = get_fd_from_conn_intf(conn_intf);
  struct conn_info_t* conn_info = get_conn_info(tgid, fd);
  if (conn_info == NULL) {
    return 0;
  }

  struct go_grpc_http2_header_event_t event = {};
  event.attr.type = kGRPCOperateHeaders;
  event.attr.htype = kHeaderEventRead;
  event.attr.timestamp_ns = bpf_ktime_get_ns();
  event.attr.conn_id = conn_info->conn_id;
  event.attr.stream_id = stream_id;

  // Using 'int i' below seems prevent loop getting rolled.
  // TODO(yzhao): Investigate and fix.
#pragma unroll
  for (unsigned int i = 0, pos = 0; i < HEADER_COUNT && i < fields.len; ++i) {
    fill_header_field(&event, fields_ptr + i);
    go_grpc_header_events.perf_submit(ctx, &event, sizeof(event));
  }

  return 0;
}

// Probe for the http2 library's frame writer.
//
// Function signature:
//   func (f *Framer) WriteData(streamID uint32, endStream bool, data []byte) error
//   func (f *Framer) WriteDataPadded(streamID uint32, endStream bool, data, pad []byte) error
//
// Verified to be stable from go1.7 to t go.1.13.
// This probe extracts the header field, which is the 1st argument.
int probe_framer_write_data(struct pt_regs* ctx) {
  const char* sp = (const char*)ctx->sp;
  if (sp == NULL) {
    return 0;
  }

  void* framer_ptr = *(void**)(sp + 8);
  uint32_t stream_id = *(uint32_t*)(sp + 16);
  struct go_byte_array data = *(struct go_byte_array*)(sp + 24);

  struct go_grpc_data_event_t* info = get_data_event();
  if (info == NULL) {
    return 0;
  }

  info->attr.type = kWriteData;

  u32 tgid = bpf_get_current_pid_tgid() >> 32;
  u32 fd = get_fd_from_http2_framer(framer_ptr);
  struct conn_info_t* conn_info = get_conn_info(tgid, fd);
  if (conn_info == NULL) {
    return 0;
  }

  info->attr.type = kWriteData;
  info->attr.ftype = kDataFrameEventWrite;
  info->attr.timestamp_ns = bpf_ktime_get_ns();
  info->attr.conn_id = conn_info->conn_id;
  info->attr.stream_id = stream_id;
  uint32_t data_len = BPF_LEN_CAP(data.len, MAX_DATA_SIZE);
  info->attr.data_len = data_len;
  bpf_probe_read(info->data, info->attr.data_len, data.ptr);

  // Replacing data_len with info->attr.data_len causes BPF verifier to reject the statement below.
  // Possibly because it lost track of the value because of the indirect access to info->attr.
  // TODO(yzhao): This could be one reason to prefer flat data structure to grouped attributes,
  // as its simplicity is more friendly to BPF verifier.
  go_grpc_data_events.perf_submit(ctx, info, sizeof(info->attr) + data_len);

  return 0;
}

// Probe for the http2 library's frame reader.
// As a proxy for the return probe on ReadFrame(), we currently probe checkFrameOrder,
// since return probes don't work for Go.
//
// Function signature:
//   func (fr *Framer) checkFrameOrder(f Frame) error
//
// Verified to be stable from at least go1.6 to t go.1.13.
int probe_framer_check_frame_order(struct pt_regs* ctx) {
  const char* sp = (const char*)ctx->sp;
  if (sp == NULL) {
    return 0;
  }

  // Param 0 is the Receiver (fr *Framer).
  const int kParam0Offset = 8;

  // Param 1 is the first argument (f Frame).
  const int kParam1Offset = 16;

  struct go_interface* frame_interface_ptr = (struct go_interface*)(sp + kParam1Offset);

  // All frames types start with a frame header, so this is safe.
  struct FrameHeader* frame_header_ptr = (struct FrameHeader*)frame_interface_ptr->ptr;

  // Consider only data frames (0).
  if (frame_header_ptr->frame_type == 0) {
    void* framer_ptr = *(void**)(sp + kParam0Offset);
    // Reinterpret as data frame.
    struct DataFrame* data_frame_ptr = (struct DataFrame*)frame_header_ptr;
    struct go_byte_array data = data_frame_ptr->data;

    struct go_grpc_data_event_t* info = get_data_event();
    if (info == NULL) {
      return 0;
    }

    info->attr.type = kReadData;

    u32 tgid = bpf_get_current_pid_tgid() >> 32;
    u32 fd = get_fd_from_http2_framer(framer_ptr);
    struct conn_info_t* conn_info = get_conn_info(tgid, fd);
    if (conn_info == NULL) {
      return 0;
    }
    info->attr.conn_id = conn_info->conn_id;

    info->attr.timestamp_ns = bpf_ktime_get_ns();
    info->attr.type = kReadData;
    info->attr.ftype = kDataFrameEventRead;
    info->attr.timestamp_ns = bpf_ktime_get_ns();
    info->attr.conn_id = conn_info->conn_id;
    info->attr.stream_id = frame_header_ptr->stream_id;
    uint32_t data_len = BPF_LEN_CAP(data.len, MAX_DATA_SIZE);
    info->attr.data_len = data_len;
    bpf_probe_read(info->data, data_len, data.ptr);

    go_grpc_data_events.perf_submit(ctx, info, sizeof(info->attr) + data_len);
  }

  return 0;
}
