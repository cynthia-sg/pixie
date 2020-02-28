#include "src/stirling/cql/cql_stitcher.h"

#include <deque>
#include <string>
#include <utility>

#include "src/common/base/base.h"
#include "src/common/json/json.h"
#include "src/stirling/cql/frame_body_decoder.h"
#include "src/stirling/cql/types.h"

namespace pl {
namespace stirling {
namespace cass {

namespace {
std::string BytesToString(std::basic_string_view<uint8_t> x) {
  return pl::BytesToString<PrintStyle::kHexCompact>(CreateStringView<char>(x));
}
}  // namespace

void CheckReqRespPair(const Record& r) {
  PL_UNUSED(r);
  // TODO(oazizi): Add some checks here.
}

Status ProcessStartupReq(Frame* req_frame, Request* req) {
  FrameBodyDecoder decoder(*req_frame);
  PL_ASSIGN_OR_RETURN(StringMap options, decoder.ExtractStringMap());
  PL_RETURN_IF_ERROR(decoder.ExpectEOF());

  DCHECK(req->msg.empty());
  req->msg = utils::ToJSONString(options);

  return Status::OK();
}

Status ProcessAuthResponseReq(Frame* req_frame, Request* req) {
  FrameBodyDecoder decoder(*req_frame);
  PL_ASSIGN_OR_RETURN(std::basic_string<uint8_t> token, decoder.ExtractBytes());
  PL_RETURN_IF_ERROR(decoder.ExpectEOF());

  std::string_view token_str = CreateStringView<char>(token);

  DCHECK(req->msg.empty());
  req->msg = token_str;

  return Status::OK();
}

Status ProcessOptionsReq(Frame* req_frame, Request* req) {
  FrameBodyDecoder decoder(*req_frame);
  PL_RETURN_IF_ERROR(decoder.ExpectEOF());

  DCHECK(req->msg.empty());

  return Status::OK();
}

Status ProcessRegisterReq(Frame* req_frame, Request* req) {
  FrameBodyDecoder decoder(*req_frame);
  PL_ASSIGN_OR_RETURN(StringList event_types, decoder.ExtractStringList());
  PL_RETURN_IF_ERROR(decoder.ExpectEOF());

  DCHECK(req->msg.empty());
  req->msg = utils::ToJSONString(event_types);

  return Status::OK();
}

Status ProcessQueryReq(Frame* req_frame, Request* req) {
  FrameBodyDecoder decoder(*req_frame);
  PL_ASSIGN_OR_RETURN(std::string query, decoder.ExtractLongString());
  PL_ASSIGN_OR_RETURN(QueryParameters qp, decoder.ExtractQueryParameters());
  PL_RETURN_IF_ERROR(decoder.ExpectEOF());

  // TODO(oazizi): This is just a placeholder.
  // Real implementation should figure out what type each value is, and cast into the appropriate
  // type. This, however, will be hard unless we have observed the preceding Prepare request.
  std::vector<std::string> hex_values;
  for (const auto& value_i : qp.values) {
    hex_values.push_back(BytesToString(value_i.value));
  }

  DCHECK(req->msg.empty());
  req->msg = query;

  // For now, just tag the parameter values to the end.
  // TODO(oazizi): Make this prettier.
  if (!hex_values.empty()) {
    absl::StrAppend(&req->msg, "\n");
    absl::StrAppend(&req->msg, utils::ToJSONString(hex_values));
  }

  return Status::OK();
}

Status ProcessPrepareReq(Frame* req_frame, Request* req) {
  FrameBodyDecoder decoder(*req_frame);
  PL_ASSIGN_OR_RETURN(std::string query, decoder.ExtractLongString());
  PL_RETURN_IF_ERROR(decoder.ExpectEOF());

  DCHECK(req->msg.empty());
  req->msg = query;

  return Status::OK();
}

Status ProcessExecuteReq(Frame* req_frame, Request* req) {
  FrameBodyDecoder decoder(*req_frame);
  PL_ASSIGN_OR_RETURN(std::basic_string<uint8_t> id, decoder.ExtractShortBytes());
  PL_ASSIGN_OR_RETURN(QueryParameters qp, decoder.ExtractQueryParameters());
  PL_RETURN_IF_ERROR(decoder.ExpectEOF());

  // TODO(oazizi): This is just a placeholder.
  // Real implementation should figure out what type each value is, and cast into the appropriate
  // type. This, however, will be hard unless we have observed the preceding Prepare request.
  std::vector<std::string> hex_values;
  for (const auto& value_i : qp.values) {
    hex_values.push_back(BytesToString(value_i.value));
  }

  DCHECK(req->msg.empty());
  req->msg = utils::ToJSONString(hex_values);

  return Status::OK();
}

struct BatchQuery {
  uint8_t kind;
  std::variant<std::string, std::basic_string<uint8_t>> query_or_id;
  std::vector<NameValuePair> values;
};

struct Batch {
  uint8_t type;
  std::vector<BatchQuery> queries;
  uint16_t consistency;
  uint8_t flags;
  uint16_t serial_consistency;
  int64_t timestamp;
};

Status ProcessBatchReq(Frame* req_frame, Request* req) {
  Batch b;

  FrameBodyDecoder decoder(*req_frame);
  PL_ASSIGN_OR_RETURN(b.type, decoder.ExtractByte());
  // - If <type> == 0, the batch will be "logged". This is equivalent to a
  //   normal CQL3 batch statement.
  // - If <type> == 1, the batch will be "unlogged".
  // - If <type> == 2, the batch will be a "counter" batch (and non-counter
  //   statements will be rejected).
  if (b.type > 2) {
    return error::Internal("Unrecognized BATCH type");
  }
  PL_ASSIGN_OR_RETURN(uint16_t n, decoder.ExtractShort());

  for (uint i = 0; i < n; ++i) {
    BatchQuery q;
    PL_ASSIGN_OR_RETURN(q.kind, decoder.ExtractByte());
    if (q.kind == 0) {
      PL_ASSIGN_OR_RETURN(q.query_or_id, decoder.ExtractLongString());
    } else if (q.kind == 1) {
      PL_ASSIGN_OR_RETURN(q.query_or_id, decoder.ExtractShortBytes());
    }
    // See note below about flag_with_names_for_values.
    PL_ASSIGN_OR_RETURN(q.values, decoder.ExtractNameValuePairList(false));
    b.queries.push_back(std::move(q));
  }

  PL_ASSIGN_OR_RETURN(b.consistency, decoder.ExtractShort());
  PL_ASSIGN_OR_RETURN(b.flags, decoder.ExtractByte());

  bool flag_with_serial_consistency = b.flags & 0x10;
  bool flag_with_default_timestamp = b.flags & 0x20;
  bool flag_with_names_for_values = b.flags & 0x40;

  // Note that the flag `with_names_for_values` occurs after its use in the spec,
  // that's why we have hard-coded the value to false in the call to ExtractNameValuePairList()
  // above. This is actually what the spec defines, because of the spec bug:
  //
  // With names for values. If set, then all values for all <query_i> must be
  // preceded by a [string] <name_i> that have the same meaning as in QUERY
  // requests [IMPORTANT NOTE: this feature does not work and should not be
  // used. It is specified in a way that makes it impossible for the server
  // to implement. This will be fixed in a future version of the native
  // protocol. See https://issues.apache.org/jira/browse/CASSANDRA-10246 for
  // more details].
  PL_UNUSED(flag_with_names_for_values);

  if (flag_with_serial_consistency) {
    PL_ASSIGN_OR_RETURN(b.serial_consistency, decoder.ExtractShort());
  }

  if (flag_with_default_timestamp) {
    PL_ASSIGN_OR_RETURN(b.timestamp, decoder.ExtractLong());
  }

  PL_RETURN_IF_ERROR(decoder.ExpectEOF());

  // TODO(oazizi): Should we add other fields?

  std::vector<std::pair<std::string, std::string>> tmp;

  for (const auto& q : b.queries) {
    if (q.kind == 0) {
      tmp.push_back({"query", std::get<std::string>(q.query_or_id)});
    } else {
      tmp.push_back({"id", BytesToString(std::get<std::basic_string<uint8_t>>(q.query_or_id))});
    }
  }

  req->msg = utils::ToJSONString(tmp);

  return Status::OK();
}

Status ProcessErrorResp(Frame* resp_frame, Response* resp) {
  FrameBodyDecoder decoder(*resp_frame);
  PL_ASSIGN_OR_RETURN(int32_t error_code, decoder.ExtractInt());
  PL_ASSIGN_OR_RETURN(std::string error_msg, decoder.ExtractString());
  PL_RETURN_IF_ERROR(decoder.ExpectEOF());

  DCHECK(resp->msg.empty());
  resp->msg = absl::Substitute("[$0] $1", error_code, error_msg);

  return Status::OK();
}

Status ProcessReadyResp(Frame* resp_frame, Response* resp) {
  FrameBodyDecoder decoder(*resp_frame);
  PL_RETURN_IF_ERROR(decoder.ExpectEOF());

  DCHECK(resp->msg.empty());

  return Status::OK();
}

Status ProcessSupportedResp(Frame* resp_frame, Response* resp) {
  FrameBodyDecoder decoder(*resp_frame);
  PL_ASSIGN_OR_RETURN(StringMultiMap options, decoder.ExtractStringMultiMap());
  PL_RETURN_IF_ERROR(decoder.ExpectEOF());

  DCHECK(resp->msg.empty());
  resp->msg = utils::ToJSONString(options);

  return Status::OK();
}

Status ProcessAuthenticateResp(Frame* resp_frame, Response* resp) {
  FrameBodyDecoder decoder(*resp_frame);
  PL_ASSIGN_OR_RETURN(std::string authenticator_name, decoder.ExtractString());
  PL_RETURN_IF_ERROR(decoder.ExpectEOF());

  DCHECK(resp->msg.empty());
  resp->msg = std::move(authenticator_name);

  return Status::OK();
}

Status ProcessAuthSuccessResp(Frame* resp_frame, Response* resp) {
  FrameBodyDecoder decoder(*resp_frame);
  PL_ASSIGN_OR_RETURN(std::basic_string<uint8_t> token, decoder.ExtractBytes());
  PL_RETURN_IF_ERROR(decoder.ExpectEOF());

  std::string token_hex = BytesToString(token);

  DCHECK(resp->msg.empty());
  resp->msg = token_hex;

  return Status::OK();
}

Status ProcessAuthChallengeResp(Frame* resp_frame, Response* resp) {
  FrameBodyDecoder decoder(*resp_frame);
  PL_ASSIGN_OR_RETURN(std::basic_string<uint8_t> token, decoder.ExtractBytes());
  PL_RETURN_IF_ERROR(decoder.ExpectEOF());

  std::string token_hex = BytesToString(token);

  DCHECK(resp->msg.empty());
  resp->msg = token_hex;

  return Status::OK();
}

Status ProcessResultVoid(FrameBodyDecoder* decoder, Response* resp) {
  PL_RETURN_IF_ERROR(decoder->ExpectEOF());

  DCHECK(resp->msg.empty());
  resp->msg = "Response type = VOID";
  return Status::OK();
}

// See section 4.2.5.2 of the spec.
Status ProcessResultRows(FrameBodyDecoder* decoder, Response* resp) {
  PL_ASSIGN_OR_RETURN(ResultMetadata metadata, decoder->ExtractResultMetadata());
  PL_ASSIGN_OR_RETURN(int32_t rows_count, decoder->ExtractInt());
  // Skip grabbing the row content for now.
  // PL_RETURN_IF_ERROR(decoder->ExpectEOF());

  // Copy to vector so we can use ToJSONString().
  // TODO(oazizi): Find a cleaner way. This is temporary anyways.
  std::vector<std::string> names;
  for (auto& c : metadata.col_specs) {
    names.push_back(std::move(c.name));
  }

  DCHECK(resp->msg.empty());
  resp->msg = absl::StrCat("Response type = ROWS\n", "Number of columns = ", metadata.columns_count,
                           "\n", utils::ToJSONString(names), "\n", "Number of rows = ", rows_count);
  // TODO(oazizi): Consider which other parts of metadata would be interesting to record into resp.

  return Status::OK();
}

Status ProcessResultSetKeyspace(FrameBodyDecoder* decoder, Response* resp) {
  PL_ASSIGN_OR_RETURN(std::string keyspace_name, decoder->ExtractString());
  PL_RETURN_IF_ERROR(decoder->ExpectEOF());

  DCHECK(resp->msg.empty());
  resp->msg = absl::StrCat("Response type = SET_KEYSPACE\n", "Keyspace = ", keyspace_name);
  return Status::OK();
}

Status ProcessResultPrepared(FrameBodyDecoder* decoder, Response* resp) {
  PL_ASSIGN_OR_RETURN(std::basic_string<uint8_t> id, decoder->ExtractShortBytes());
  // Note that two metadata are sent back. The first communicates the col specs for the Prepared
  // statement, while the second communicates the metadata for future EXECUTE statements.
  PL_ASSIGN_OR_RETURN(ResultMetadata metadata, decoder->ExtractResultMetadata(/* has_pk */ true));
  PL_ASSIGN_OR_RETURN(ResultMetadata result_metadata, decoder->ExtractResultMetadata());
  PL_RETURN_IF_ERROR(decoder->ExpectEOF());

  DCHECK(resp->msg.empty());
  resp->msg = "Response type = PREPARED";
  // TODO(oazizi): Add more information.

  return Status::OK();
}

Status ProcessResultSchemaChange(FrameBodyDecoder* decoder, Response* resp) {
  PL_ASSIGN_OR_RETURN(SchemaChange sc, decoder->ExtractSchemaChange());
  PL_RETURN_IF_ERROR(decoder->ExpectEOF());

  DCHECK(resp->msg.empty());
  resp->msg = "Response type = SCHEMA_CHANGE";
  // TODO(oazizi): Add more information.

  return Status::OK();
}

Status ProcessResultResp(Frame* resp_frame, Response* resp) {
  FrameBodyDecoder decoder(*resp_frame);
  PL_ASSIGN_OR_RETURN(int32_t kind, decoder.ExtractInt());

  switch (kind) {
    case 0x0001:
      return ProcessResultVoid(&decoder, resp);
    case 0x0002:
      return ProcessResultRows(&decoder, resp);
    case 0x0003:
      return ProcessResultSetKeyspace(&decoder, resp);
    case 0x0004:
      return ProcessResultPrepared(&decoder, resp);
    case 0x0005:
      return ProcessResultSchemaChange(&decoder, resp);
    default:
      return error::Internal("Unrecognized result kind (%d)", kind);
  }
}

Status ProcessEventResp(Frame* resp_frame, Response* resp) {
  FrameBodyDecoder decoder(*resp_frame);
  PL_ASSIGN_OR_RETURN(std::string event_type, decoder.ExtractString());

  if (event_type == "TOPOLOGY_CHANGE" || event_type == "STATUS_CHANGE") {
    PL_ASSIGN_OR_RETURN(std::string change_type, decoder.ExtractString());
    PL_ASSIGN_OR_RETURN(SockAddr addr, decoder.ExtractInet());
    PL_RETURN_IF_ERROR(decoder.ExpectEOF());

    DCHECK(resp->msg.empty());
    resp->msg = absl::StrCat(event_type, " ", change_type, " ", addr.AddrStr());

    return Status::OK();
  } else if (event_type == "SCHEMA_CHANGE") {
    PL_ASSIGN_OR_RETURN(SchemaChange sc, decoder.ExtractSchemaChange());
    PL_RETURN_IF_ERROR(decoder.ExpectEOF());

    DCHECK(resp->msg.empty());
    resp->msg =
        absl::StrCat(event_type, " ", sc.change_type, " keyspace=", sc.keyspace, " name=", sc.name);
    // TODO(oazizi): Add sc.arg_types to the response string.

    return Status::OK();
  } else {
    return error::Internal("Unknown event_type $0", event_type);
  }
}

Status ProcessReq(Frame* req_frame, Request* req) {
  req->op = static_cast<ReqOp>(req_frame->hdr.opcode);
  req->timestamp_ns = req_frame->timestamp_ns;

  switch (req->op) {
    case ReqOp::kStartup:
      return ProcessStartupReq(req_frame, req);
    case ReqOp::kAuthResponse:
      return ProcessAuthResponseReq(req_frame, req);
    case ReqOp::kOptions:
      return ProcessOptionsReq(req_frame, req);
    case ReqOp::kQuery:
      return ProcessQueryReq(req_frame, req);
    case ReqOp::kPrepare:
      return ProcessPrepareReq(req_frame, req);
    case ReqOp::kExecute:
      return ProcessExecuteReq(req_frame, req);
    case ReqOp::kBatch:
      return ProcessBatchReq(req_frame, req);
    case ReqOp::kRegister:
      return ProcessRegisterReq(req_frame, req);
    default:
      return error::Internal("Unhandled opcode $0", magic_enum::enum_name(req->op));
  }
}

Status ProcessResp(Frame* resp_frame, Response* resp) {
  resp->op = static_cast<RespOp>(resp_frame->hdr.opcode);
  resp->timestamp_ns = resp_frame->timestamp_ns;

  switch (resp->op) {
    case RespOp::kError:
      return ProcessErrorResp(resp_frame, resp);
    case RespOp::kReady:
      return ProcessReadyResp(resp_frame, resp);
    case RespOp::kAuthenticate:
      return ProcessAuthenticateResp(resp_frame, resp);
    case RespOp::kSupported:
      return ProcessSupportedResp(resp_frame, resp);
    case RespOp::kResult:
      return ProcessResultResp(resp_frame, resp);
    case RespOp::kEvent:
      return ProcessEventResp(resp_frame, resp);
    case RespOp::kAuthChallenge:
      return ProcessAuthChallengeResp(resp_frame, resp);
    case RespOp::kAuthSuccess:
      return ProcessAuthSuccessResp(resp_frame, resp);
    default:
      return error::Internal("Unhandled opcode $0", magic_enum::enum_name(resp->op));
  }
}

StatusOr<Record> ProcessReqRespPair(Frame* req_frame, Frame* resp_frame) {
  ECHECK_LT(req_frame->timestamp_ns, resp_frame->timestamp_ns);

  Record r;
  PL_RETURN_IF_ERROR(ProcessReq(req_frame, &r.req));
  PL_RETURN_IF_ERROR(ProcessResp(resp_frame, &r.resp));

  CheckReqRespPair(r);
  return r;
}

StatusOr<Record> ProcessSolitaryResp(Frame* resp_frame) {
  Record r;

  // For now, Event is the only supported solitary response.
  // If this ever changes, the code below needs to be adapted.
  DCHECK(resp_frame->hdr.opcode == Opcode::kEvent);

  // Make a fake request to go along with the response.
  // - Use REGISTER op, since that was what set up the events in the first place.
  // - Use response timestamp, so any calculated latencies are reported as 0.
  r.req.op = ReqOp::kRegister;
  r.req.msg = "-";
  r.req.timestamp_ns = resp_frame->timestamp_ns;

  // A little inefficient because it will go through a switch statement again,
  // when we actually know the op. But keep it this way for consistency.
  PL_RETURN_IF_ERROR(ProcessResp(resp_frame, &r.resp));

  CheckReqRespPair(r);
  return r;
}

// Currently ProcessFrames() uses a response-led matching algorithm.
// For each response that is at the head of the deque, there should exist a previous request with
// the same stream. Find it, and consume both frames.
// TODO(oazizi): Does it make sense to sort to help the matching?
// Considerations:
//  - Request and response deques are likely (confirm?) to be mostly ordered.
//  - Stream values can be re-used, so sorting would have to consider times too.
//  - Stream values need not be in any sequential order.
std::vector<Record> ProcessFrames(std::deque<Frame>* req_frames, std::deque<Frame>* resp_frames) {
  std::vector<Record> entries;

  for (auto& resp_frame : *resp_frames) {
    bool found_match = false;

    // Event responses are special: they have no request.
    if (resp_frame.hdr.opcode == Opcode::kEvent) {
      StatusOr<Record> record_status = ProcessSolitaryResp(&resp_frame);
      if (record_status.ok()) {
        entries.push_back(record_status.ConsumeValueOrDie());
      } else {
        LOG(ERROR) << record_status.msg();
      }
      resp_frames->pop_front();
      continue;
    }

    // Search for matching req frame
    for (auto& req_frame : *req_frames) {
      if (resp_frame.hdr.stream == req_frame.hdr.stream) {
        VLOG(2) << absl::Substitute("req_op=$0 msg=$1", magic_enum::enum_name(req_frame.hdr.opcode),
                                    req_frame.msg);

        StatusOr<Record> record_status = ProcessReqRespPair(&req_frame, &resp_frame);
        if (record_status.ok()) {
          entries.push_back(record_status.ConsumeValueOrDie());
        } else {
          LOG(ERROR) << record_status.ToString();
        }

        // Found a match, so remove both request and response.
        // We don't remove request frames on the fly, however,
        // because it could otherwise cause unnecessary churn/copying in the deque.
        // This is due to the fact that responses can come out-of-order.
        // Just mark the request as consumed, and clean-up when they reach the head of the queue.
        // Note that responses are always head-processed, so they don't require this optimization.
        found_match = true;
        resp_frames->pop_front();
        req_frame.consumed = true;
        break;
      }
    }

    LOG_IF(ERROR, !found_match) << absl::Substitute(
        "Did not find a request matching the response. Stream = $0", resp_frame.hdr.stream);

    // Clean-up consumed frames at the head.
    // Do this inside the resp loop to aggressively clean-out req_frames whenever a frame consumed.
    // Should speed up the req_frames search for the next iteration.
    for (auto& req_frame : *req_frames) {
      if (!req_frame.consumed) {
        break;
      }
      req_frames->pop_front();
    }

    // TODO(oazizi): Consider removing requests that are too old, otherwise a lost response can mean
    // the are never processed. This would result in a memory leak until the more drastic connection
    // tracker clean-up mechanisms kick in.
  }

  return entries;
}

}  // namespace cass
}  // namespace stirling
}  // namespace pl
