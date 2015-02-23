#include "util/etcd.h"

#include <ctime>
#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <glog/logging.h>
#include <utility>

#include "util/json_wrapper.h"
#include "util/statusor.h"

namespace libevent = cert_trans::libevent;

using std::atoll;
using std::bind;
using std::chrono::seconds;
using std::chrono::system_clock;
using std::ctime;
using std::lock_guard;
using std::make_pair;
using std::make_shared;
using std::map;
using std::max;
using std::move;
using std::mutex;
using std::ostringstream;
using std::pair;
using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;
using std::shared_ptr;
using std::string;
using std::time_t;
using std::to_string;
using std::unique_ptr;
using std::vector;
using util::Status;
using util::StatusOr;
using util::Task;
using util::TaskHold;

DEFINE_int32(etcd_watch_error_retry_delay_seconds, 5,
             "delay between retrying etcd watch requests");
DEFINE_bool(etcd_consistent, true, "Add consistent=true param to all requests. "
            "Do not turn this off unless you *know* what you're doing.");
DEFINE_bool(etcd_quorum, true, "Add quorum=true param to all requests. "
            "Do not turn this off unless you *know* what you're doing.");
DEFINE_int32(etcd_connection_timeout_seconds, 10,
             "Number of seconds after which to timeout etcd connections.");

namespace cert_trans {

namespace {


string MessageFromJsonStatus(const shared_ptr<JsonObject>& json) {
  string message;
  const JsonString m(*json, "message");

  if (!m.Ok()) {
    message = json->DebugString();
  } else {
    message = m.Value();
  }

  return message;
}


util::error::Code ErrorCodeForHttpResponseCode(int response_code) {
  switch (response_code) {
    case 200:
    case 201:
      return util::error::OK;
    case 403:
      return util::error::PERMISSION_DENIED;
    case 404:
      return util::error::NOT_FOUND;
    case 412:
      return util::error::FAILED_PRECONDITION;
    case 500:
      return util::error::UNAVAILABLE;
    default:
      return util::error::UNKNOWN;
  }
}


bool KeyIsDirectory(const string& key) {
  return key.size() > 0 && key.back() == '/';
}


Status StatusFromResponseCode(const int response_code,
                              const shared_ptr<JsonObject>& json) {
  const util::error::Code error_code(
      ErrorCodeForHttpResponseCode(response_code));
  const string error_message(
      error_code == util::error::OK ? "" : MessageFromJsonStatus(json));
  return Status(error_code, error_message);
}


shared_ptr<evhttp_uri> UriFromHostPort(const string& host, uint16_t port) {
  const shared_ptr<evhttp_uri> retval(CHECK_NOTNULL(evhttp_uri_new()),
                                      evhttp_uri_free);

  evhttp_uri_set_scheme(retval.get(), "http");
  evhttp_uri_set_host(retval.get(), host.c_str());
  evhttp_uri_set_port(retval.get(), port);

  return retval;
}


void GetRequestDone(EtcdClient::GenericResponse* gen_resp,
                    const EtcdClient::GetCallback& cb, Task* task) {
  const unique_ptr<EtcdClient::GenericResponse> gen_resp_deleter(gen_resp);
  const unique_ptr<Task> task_deleter(task);

  if (!task->status().ok()) {
    cb(task->status(), EtcdClient::Node::InvalidNode(), -1);
    return;
  }

  const JsonObject node(*gen_resp->json_body, "node");
  if (!node.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'node'"),
       EtcdClient::Node::InvalidNode(), -1);
    return;
  }

  const JsonInt createdIndex(node, "createdIndex");
  if (!createdIndex.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'createdIndex'"),
       EtcdClient::Node::InvalidNode(), -1);
    return;
  }

  const JsonInt modifiedIndex(node, "modifiedIndex");
  if (!modifiedIndex.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'modifiedIndex'"),
       EtcdClient::Node::InvalidNode(), -1);
    return;
  }

  const JsonString key(node, "key");
  if (!key.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'key'"),
       EtcdClient::Node::InvalidNode(), -1);
    return;
  }

  const JsonString value(node, "value");
  if (!value.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'value'"),
       EtcdClient::Node::InvalidNode(), -1);
    return;
  }

  cb(Status::OK, EtcdClient::Node(createdIndex.Value(), modifiedIndex.Value(),
                                  key.Value(), value.Value()),
     gen_resp->etcd_index);
}


void GetAllRequestDone(EtcdClient::GenericResponse* gen_resp,
                       const EtcdClient::GetAllCallback& cb, Task* task) {
  const unique_ptr<EtcdClient::GenericResponse> gen_resp_deleter(gen_resp);
  const unique_ptr<Task> task_deleter(task);

  if (!task->status().ok()) {
    cb(task->status(), vector<EtcdClient::Node>(), -1);
    return;
  }

  const JsonObject node(*gen_resp->json_body, "node");
  if (!node.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'node'"),
       vector<EtcdClient::Node>(), -1);
    return;
  }

  const JsonBoolean isDir(node, "dir");
  if (!isDir.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'dir'"),
       vector<EtcdClient::Node>(), -1);
  }

  if (!isDir.Value()) {
    cb(Status(util::error::INVALID_ARGUMENT, "Not a directory"),
       vector<EtcdClient::Node>(), -1);
  }

  const JsonArray value_nodes(node, "nodes");
  if (!value_nodes.Ok()) {
    // Directory is empty.
    cb(Status::OK, vector<EtcdClient::Node>(), -1);
    return;
  }

  vector<EtcdClient::Node> values;
  for (int i = 0; i < value_nodes.Length(); ++i) {
    const JsonObject entry(value_nodes, i);
    if (!entry.Ok()) {
      cb(Status(util::error::FAILED_PRECONDITION,
                "Invalid JSON: Couldn't get 'value_nodes' index " +
                    to_string(i)),
         vector<EtcdClient::Node>(), -1);
      return;
    }

    const JsonString value(entry, "value");
    if (!value.Ok()) {
      cb(Status(util::error::FAILED_PRECONDITION,
                "Invalid JSON: Couldn't find 'value'"),
         vector<EtcdClient::Node>(), -1);
      return;
    }

    const JsonInt createdIndex(entry, "createdIndex");
    if (!createdIndex.Ok()) {
      cb(Status(util::error::FAILED_PRECONDITION,
                "Invalid JSON: Coulnd't find 'createdIndex'"),
         vector<EtcdClient::Node>(), -1);
      return;
    }

    const JsonInt modifiedIndex(entry, "modifiedIndex");
    if (!modifiedIndex.Ok()) {
      cb(Status(util::error::FAILED_PRECONDITION,
                "Invalid JSON: Coulnd't find 'modifiedIndex'"),
         vector<EtcdClient::Node>(), -1);
      return;
    }

    const JsonString key(entry, "key");
    if (!key.Ok()) {
      cb(Status(util::error::FAILED_PRECONDITION,
                "Invalid JSON: Couldn't find 'key'"),
         vector<EtcdClient::Node>(), -1);
      return;
    }

    values.emplace_back(EtcdClient::Node(createdIndex.Value(),
                                         modifiedIndex.Value(), key.Value(),
                                         value.Value()));
  }

  cb(Status::OK, values, gen_resp->etcd_index);
}


void CreateRequestDone(EtcdClient::GenericResponse* gen_resp,
                       const EtcdClient::CreateCallback& cb, Task* task) {
  const unique_ptr<EtcdClient::GenericResponse> gen_resp_deleter(gen_resp);
  const unique_ptr<Task> task_deleter(task);

  if (!task->status().ok()) {
    cb(task->status(), -1);
    return;
  }

  const JsonObject node(*gen_resp->json_body, "node");
  if (!node.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'node'"),
       0);
    return;
  }

  const JsonInt createdIndex(node, "createdIndex");
  if (!createdIndex.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'createdIndex'"),
       0);
    return;
  }

  const JsonInt modifiedIndex(node, "modifiedIndex");
  if (!modifiedIndex.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'modifiedIndex'"),
       0);
    return;
  }

  CHECK_EQ(createdIndex.Value(), modifiedIndex.Value());
  cb(Status::OK, modifiedIndex.Value());
}


void CreateInQueueRequestDone(EtcdClient::GenericResponse* gen_resp,
                              const EtcdClient::CreateInQueueCallback& cb,
                              Task* task) {
  const unique_ptr<EtcdClient::GenericResponse> gen_resp_deleter(gen_resp);
  const unique_ptr<Task> task_deleter(task);

  if (!task->status().ok()) {
    cb(task->status(), "", -1);
    return;
  }

  const JsonObject node(*gen_resp->json_body, "node");
  if (!node.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'node'"),
       "", 0);
    return;
  }

  const JsonInt createdIndex(node, "createdIndex");
  if (!createdIndex.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'createdIndex'"),
       "", 0);
    return;
  }

  const JsonInt modifiedIndex(node, "modifiedIndex");
  if (!modifiedIndex.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'modifiedIndex'"),
       "", 0);
    return;
  }

  const JsonString key(node, "key");
  if (!key.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'key'"),
       "", 0);
    return;
  }

  CHECK_EQ(createdIndex.Value(), modifiedIndex.Value());
  cb(Status::OK, key.Value(), modifiedIndex.Value());
}


void UpdateRequestDone(EtcdClient::GenericResponse* gen_resp,
                       const EtcdClient::UpdateCallback& cb, Task* task) {
  const unique_ptr<EtcdClient::GenericResponse> gen_resp_deleter(gen_resp);
  const unique_ptr<Task> task_deleter(task);

  if (!task->status().ok()) {
    cb(task->status(), -1);
    return;
  }

  const JsonObject node(*gen_resp->json_body, "node");
  if (!node.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'node'"),
       0);
    return;
  }

  const JsonInt modifiedIndex(node, "modifiedIndex");
  if (!modifiedIndex.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'modifiedIndex'"),
       0);
    return;
  }

  cb(Status::OK, modifiedIndex.Value());
}


void ForceSetRequestDone(EtcdClient::GenericResponse* gen_resp,
                         const EtcdClient::ForceSetCallback& cb, Task* task) {
  const unique_ptr<EtcdClient::GenericResponse> gen_resp_deleter(gen_resp);
  const unique_ptr<Task> task_deleter(task);

  if (!task->status().ok()) {
    cb(task->status(), -1);
    return;
  }

  const JsonObject node(*gen_resp->json_body, "node");
  if (!node.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'node'"),
       0);
    return;
  }

  const JsonInt modifiedIndex(node, "modifiedIndex");
  if (!modifiedIndex.Ok()) {
    cb(Status(util::error::FAILED_PRECONDITION,
              "Invalid JSON: Couldn't find 'modifiedIndex'"),
       0);
    return;
  }

  cb(Status::OK, modifiedIndex.Value());
}


void DeleteRequestDone(EtcdClient::GenericResponse* gen_resp,
                       const EtcdClient::DeleteCallback& cb, Task* task) {
  const unique_ptr<EtcdClient::GenericResponse> gen_resp_deleter(gen_resp);
  const unique_ptr<Task> task_deleter(task);

  if (!task->status().ok()) {
    cb(task->status(), -1);
    return;
  }

  cb(Status::OK, gen_resp->etcd_index);
}


string UrlEscapeAndJoinParams(const map<string, string>& params) {
  string retval;

  bool first(true);
  for (map<string, string>::const_iterator it = params.begin();
       it != params.end(); ++it) {
    if (first)
      first = false;
    else
      retval += "&";

    unique_ptr<char, void (*)(void*)> first(
        evhttp_uriencode(it->first.c_str(), it->first.size(), 0), &free);
    unique_ptr<char, void (*)(void*)> second(
        evhttp_uriencode(it->second.c_str(), it->second.size(), 0), &free);

    retval += first.get();
    retval += "=";
    retval += second.get();
  }

  return retval;
}


static const EtcdClient::Node kInvalidNode(-1, -1, "", "");


}  // namespace


struct EtcdClient::Request {
  Request(EtcdClient* client, evhttp_cmd_type verb, const string& key,
          bool separate_conn, const map<string, string>& params,
          GenericResponse* gen_resp, Task* task)
      : client_(client),
        verb_(verb),
        path_("/v2/keys" + key),
        separate_conn_(separate_conn),
        params_(UrlEscapeAndJoinParams(params)),
        gen_resp_(CHECK_NOTNULL(gen_resp)),
        task_(CHECK_NOTNULL(task)) {
    CHECK(!key.empty());
    CHECK_EQ(key[0], '/');
  }

  void Run(const shared_ptr<libevent::HttpConnection>& conn) {
    CHECK(!req_) << "running an already running request";
    CHECK(!conn_);

    conn_ = separate_conn_
                ? shared_ptr<libevent::HttpConnection>(conn->Clone())
                : conn;
    conn_->SetTimeout(seconds(FLAGS_etcd_connection_timeout_seconds));

    const shared_ptr<libevent::HttpRequest> req(
        make_shared<libevent::HttpRequest>(
            bind(&EtcdClient::RequestDone, client_, _1, this)));

    string uri(path_);
    if (verb_ == EVHTTP_REQ_PUT || verb_ == EVHTTP_REQ_POST) {
      evhttp_add_header(req->GetOutputHeaders(), "Content-Type",
                        "application/x-www-form-urlencoded");
      CHECK_EQ(evbuffer_add(req->GetOutputBuffer(), params_.data(),
                            params_.size()),
               0);
    } else {
      if (!params_.empty()) {
        uri += "?" + params_;
      }
    }

    {
      lock_guard<mutex> lock(lock_);
      CHECK(!req_);
      req_ = req;
    }
    conn_->MakeRequest(req, verb_, uri.c_str());
  }

  void Reset() {
    lock_guard<mutex> lock(lock_);
    req_.reset();
    conn_.reset();
  }

  EtcdClient* const client_;
  const evhttp_cmd_type verb_;
  const string path_;
  const bool separate_conn_;
  const string params_;
  GenericResponse* const gen_resp_;
  Task* const task_;

  shared_ptr<libevent::HttpConnection> conn_;

  // Only the request is protected, because everything else is
  // event-driven, and so, there is no concurrency.
  mutex lock_;
  shared_ptr<libevent::HttpRequest> req_;
};


struct EtcdClient::WatchState {
  WatchState(EtcdClient* client, const string& key, const WatchCallback& cb,
             Task* task);
  ~WatchState();

  void InitialGetDone(Status status, const Node& node, int64_t etcd_index);
  void InitialGetAllDone(Status status, const vector<Node>& nodes,
                         int64_t etcd_index);
  void RequestDone(GenericResponse* gen_resp, Task* task);
  void SendUpdates(const vector<WatchUpdate>& updates);
  void StartRequest();
  Status HandleSingleValueRequestDone(const JsonObject& node,
                                      vector<WatchUpdate>* updates);

  EtcdClient* const client_;
  const string key_;
  const WatchCallback cb_;
  Task* const task_;

  int64_t highest_index_seen_;
  EtcdClient::Request* req_;
};


EtcdClient::WatchState::WatchState(EtcdClient* client, const string& key,
                                   const WatchCallback& cb, Task* task)
    : client_(CHECK_NOTNULL(client)),
      key_(key),
      cb_(cb),
      task_(CHECK_NOTNULL(task)),
      highest_index_seen_(-1),
      req_(nullptr) {
  if (KeyIsDirectory(key_)) {
    client_->GetAll(key,
                    bind(&WatchState::InitialGetAllDone, this, _1, _2, _3));
  } else {
    client_->Get(key, bind(&WatchState::InitialGetDone, this, _1, _2, _3));
  }
}


EtcdClient::WatchState::~WatchState() {
  VLOG(1) << "EtcdClient::Watch: no longer watching " << key_;
}


EtcdClient::WatchUpdate::WatchUpdate(const Node& node, const bool exists)
    : node_(node), exists_(exists) {
}


EtcdClient::WatchUpdate::WatchUpdate()
    : node_(EtcdClient::Node::InvalidNode()), exists_(false) {
}


void EtcdClient::WatchState::InitialGetDone(Status status, const Node& node,
                                            int64_t etcd_index) {
  InitialGetAllDone(status, {node}, etcd_index);
}


void EtcdClient::WatchState::InitialGetAllDone(Status status,
                                               const vector<Node>& nodes,
                                               int64_t etcd_index) {
  if (task_->CancelRequested()) {
    task_->Return(Status::CANCELLED);
    return;
  }

  // TODO(pphaneuf): Need better error handling here. Have to review
  // what the possible errors are, most of them should probably be
  // dealt with using retries?
  CHECK(status.ok()) << "initial get error: " << status;

  highest_index_seen_ = etcd_index;

  vector<WatchUpdate> updates;
  for (const auto& node : nodes) {
    updates.emplace_back(WatchUpdate(node, true /*exists*/));
  }

  task_->executor()->Add(bind(cb_, move(updates)));

  StartRequest();
}


StatusOr<EtcdClient::WatchUpdate> UpdateForNode(const JsonObject& node) {
  const JsonInt createdIndex(node, "createdIndex");
  if (!createdIndex.Ok()) {
    return StatusOr<EtcdClient::WatchUpdate>(
        Status(util::error::FAILED_PRECONDITION,
               "Invalid JSON: Couldn't find 'createdIndex'"));
  }

  const JsonInt modifiedIndex(node, "modifiedIndex");
  if (!modifiedIndex.Ok()) {
    return StatusOr<EtcdClient::WatchUpdate>(
        Status(util::error::FAILED_PRECONDITION,
               "Invalid JSON: Couldn't find 'modifiedIndex'"));
  }

  const JsonString key(node, "key");
  if (!key.Ok()) {
    return StatusOr<EtcdClient::WatchUpdate>(
        Status(util::error::FAILED_PRECONDITION,
               "Invalid JSON: Couldn't find 'key'"));
  }

  const JsonString value(node, "value");
  if (value.Ok()) {
    return StatusOr<EtcdClient::WatchUpdate>(EtcdClient::WatchUpdate(
        EtcdClient::Node(createdIndex.Value(), modifiedIndex.Value(),
                         key.Value(), value.Value()),
        true /*exists*/));
  } else {
    return StatusOr<EtcdClient::WatchUpdate>(EtcdClient::WatchUpdate(
        EtcdClient::Node(createdIndex.Value(), modifiedIndex.Value(),
                         key.Value(), ""),
        false /*exists*/));
  }
}


Status EtcdClient::WatchState::HandleSingleValueRequestDone(
    const JsonObject& node, vector<WatchUpdate>* updates) {
  StatusOr<WatchUpdate> status(UpdateForNode(node));
  if (!status.ok()) {
    return status.status();
  }
  updates->emplace_back(status.ValueOrDie());

  return Status::OK;
}


void EtcdClient::WatchState::RequestDone(GenericResponse* gen_resp,
                                         Task* task) {
  // We clean up this way instead of using util::Task::DeleteWhenDone,
  // because our task is long-lived, and we do not want to accumulate
  // these objects.
  unique_ptr<GenericResponse> gen_resp_deleter(gen_resp);

  VLOG(2) << "etcd_index: " << gen_resp->etcd_index;

  // TODO(alcutter): doing this here works around some etcd 401 errors, but in
  // the case of sustained high qps we could miss updates entirely (e.g. if
  // this new index is already past the 1000 entry horizon by the time we make
  // the new watch request.)  One way to address this might be to have the
  // watcher re-do an "initial" get on the target, and, in the case of
  // directory watches, maintain a set of known keys so that it can synthesise
  // 'delete' updates.
  CHECK_LE(highest_index_seen_, gen_resp->etcd_index);
  highest_index_seen_ = gen_resp->etcd_index;

  if (task_->CancelRequested()) {
    task_->Return(Status::CANCELLED);
    return;
  }

  // TODO(pphaneuf): This and many other of the callbacks in this file
  // do very similar validation, we should pull that out in a shared
  // helper function.
  {
    // This is probably due to a timeout, just retry.
    if (!task->status().ok()) {
      LOG(INFO) << "Watch request failed: " << task->status();
      goto fail;
    }

    // TODO(pphaneuf): None of this should ever happen, so I'm not
    // sure what's the best way to handle it? CHECK-fail? Retry? With
    // a delay? Retrying for now...
    const JsonObject node(*gen_resp->json_body, "node");
    if (!node.Ok()) {
      LOG(INFO) << "Invalid JSON: Couldn't find 'node'";
      goto fail;
    }

    vector<WatchUpdate> updates;
    Status status(HandleSingleValueRequestDone(node, &updates));
    if (!status.ok()) {
      LOG(INFO) << "HandleSingleValueRequestDone failed: " << status;
      goto fail;
    }

    return task_->executor()->Add(
        bind(&WatchState::SendUpdates, this, move(updates)));
  }

fail:
  client_->event_base_->Delay(
      seconds(FLAGS_etcd_watch_error_retry_delay_seconds),
      task_->AddChildWithExecutor(bind(&WatchState::StartRequest, this),
                                  client_->event_base_.get()));
}


void EtcdClient::WatchState::SendUpdates(const vector<WatchUpdate>& updates) {
  cb_(updates);

  // Only start the next request once the callback has return, to make
  // sure they are always delivered in order.
  StartRequest();
}


void EtcdClient::WatchState::StartRequest() {
  if (task_->CancelRequested()) {
    task_->Return(Status::CANCELLED);
    return;
  }

  map<string, string> params;
  params["wait"] = "true";
  params["waitIndex"] = to_string(highest_index_seen_ + 1);
  params["recursive"] = "true";

  GenericResponse* const gen_resp(new GenericResponse);
  client_->Generic(key_, params, EVHTTP_REQ_GET, true, gen_resp,
                   task_->AddChild(
                       bind(&WatchState::RequestDone, this, gen_resp, _1)));
}


EtcdClient::Node::Node(int64_t created_index, int64_t modified_index,
                       const string& key, const string& value)
    : created_index_(created_index),
      modified_index_(modified_index),
      key_(key),
      value_(value),
      expires_(system_clock::time_point::max()),
      deleted_(false) {
}


// static
const EtcdClient::Node& EtcdClient::Node::InvalidNode() {
  return kInvalidNode;
}


string EtcdClient::Node::ToString() const {
  ostringstream oss;
  oss << "[" << key_ << ": '" << value_ << "' c: " << created_index_
      << " m: " << modified_index_;
  if (HasExpiry()) {
    time_t time_c = system_clock::to_time_t(expires_);
    oss << " expires: " << ctime(&time_c);
  }
  oss << " deleted: " << deleted_ << "]";
  return oss.str();
}


bool EtcdClient::Node::HasExpiry() const {
  return expires_ < system_clock::time_point::max();
}


EtcdClient::EtcdClient(const shared_ptr<libevent::Base>& event_base,
                       const string& host, uint16_t port)
    : event_base_(event_base), leader_(GetConnection(host, port)) {
  CHECK_NOTNULL(event_base_.get());
  VLOG(1) << "EtcdClient: " << this;
}


EtcdClient::EtcdClient(const shared_ptr<libevent::Base>& event_base)
    : event_base_(event_base) {
  CHECK_NOTNULL(event_base_.get());
}


EtcdClient::~EtcdClient() {
  VLOG(1) << "~EtcdClient: " << this;
}


bool EtcdClient::MaybeUpdateLeader(const libevent::HttpRequest& req,
                                   Request* etcd_req) {
  // We're talking to the leader, get back to normal processing...
  if (req.GetResponseCode() != 307) {
    return false;
  }

  const char* const location(
      CHECK_NOTNULL(evhttp_find_header(req.GetInputHeaders(), "location")));

  const unique_ptr<evhttp_uri, void (*)(evhttp_uri*)> uri(
      evhttp_uri_parse(location), &evhttp_uri_free);
  CHECK(uri);
  LOG(INFO) << "etcd leader: " << evhttp_uri_get_host(uri.get()) << ":"
            << evhttp_uri_get_port(uri.get());

  // Update the last known leader, and retry the request on the new
  // leader.
  etcd_req->Run(UpdateLeader(evhttp_uri_get_host(uri.get()),
                             evhttp_uri_get_port(uri.get())));

  return true;
}


void EtcdClient::RequestDone(const shared_ptr<libevent::HttpRequest>& req,
                             Request* etcd_req) {
  // The HttpRequest object will be invalid as soon as we return, so
  // forget about it now. It's too late to cancel, anyway.
  etcd_req->Reset();

  // This can happen in the case of a timeout (not sure if there are
  // other reasons).
  if (!req) {
    etcd_req->gen_resp_->etcd_index = -1;
    etcd_req->gen_resp_->json_body = nullptr;
    etcd_req->task_->Return(Status(util::error::UNKNOWN,
                                   "evhttp request callback returned a null"));
    return;
  }

  if (MaybeUpdateLeader(*req, etcd_req)) {
    return;
  }

  const char* const etcd_index(
      evhttp_find_header(req->GetInputHeaders(), "X-Etcd-Index"));
  etcd_req->gen_resp_->json_body =
      make_shared<JsonObject>(req->GetInputBuffer());
  etcd_req->gen_resp_->etcd_index = etcd_index ? atoll(etcd_index) : -1;

  etcd_req->task_->Return(
      StatusFromResponseCode(req->GetResponseCode(),
                             etcd_req->gen_resp_->json_body));
}


shared_ptr<libevent::HttpConnection> EtcdClient::GetConnection(
    const string& host, uint16_t port) {
  const pair<string, uint16_t> host_port(make_pair(host, port));
  const ConnectionMap::const_iterator it(conns_.find(host_port));
  shared_ptr<libevent::HttpConnection> conn;

  if (it == conns_.end()) {
    conn = make_shared<libevent::HttpConnection>(event_base_,
                                                 UriFromHostPort(host, port)
                                                     .get());
    conn->SetTimeout(seconds(FLAGS_etcd_connection_timeout_seconds));
    conns_.insert(make_pair(host_port, conn));
  } else {
    conn = it->second;
  }

  return conn;
}


shared_ptr<libevent::HttpConnection> EtcdClient::GetLeader() const {
  lock_guard<mutex> lock(lock_);
  return leader_;
}


shared_ptr<libevent::HttpConnection> EtcdClient::UpdateLeader(
    const string& host, uint16_t port) {
  lock_guard<mutex> lock(lock_);
  leader_ = GetConnection(host, port);
  return leader_;
}


void EtcdClient::Get(const string& key, const GetCallback& cb) {
  map<string, string> params;
  GenericResponse* const gen_resp(new GenericResponse);
  Generic(key, params, EVHTTP_REQ_GET, false, gen_resp,
          new Task(bind(&GetRequestDone, gen_resp, cb, _1),
                   event_base_.get()));
}


void EtcdClient::GetAll(const string& dir, const GetAllCallback& cb) {
  map<string, string> params;
  GenericResponse* const gen_resp(new GenericResponse);
  Generic(dir, params, EVHTTP_REQ_GET, false, gen_resp,
          new Task(bind(&GetAllRequestDone, gen_resp, cb, _1),
                   event_base_.get()));
}


void EtcdClient::Create(const string& key, const string& value,
                        const CreateCallback& cb) {
  map<string, string> params;
  params["value"] = value;
  params["prevExist"] = "false";
  GenericResponse* const gen_resp(new GenericResponse);
  Generic(key, params, EVHTTP_REQ_PUT, false, gen_resp,
          new Task(bind(&CreateRequestDone, gen_resp, cb, _1),
                   event_base_.get()));
}


void EtcdClient::CreateWithTTL(const string& key, const string& value,
                               const seconds& ttl, const CreateCallback& cb) {
  map<string, string> params;
  params["value"] = value;
  params["prevExist"] = "false";
  params["ttl"] = to_string(ttl.count());
  GenericResponse* const gen_resp(new GenericResponse);
  Generic(key, params, EVHTTP_REQ_PUT, false, gen_resp,
          new Task(bind(&CreateRequestDone, gen_resp, cb, _1),
                   event_base_.get()));
}


void EtcdClient::CreateInQueue(const string& dir, const string& value,
                               const CreateInQueueCallback& cb) {
  map<string, string> params;
  params["value"] = value;
  params["prevExist"] = "false";
  GenericResponse* const gen_resp(new GenericResponse);
  Generic(dir, params, EVHTTP_REQ_POST, false, gen_resp,
          new Task(bind(&CreateInQueueRequestDone, gen_resp, cb, _1),
                   event_base_.get()));
}


void EtcdClient::Update(const string& key, const string& value,
                        const int64_t previous_index,
                        const UpdateCallback& cb) {
  map<string, string> params;
  params["value"] = value;
  params["prevIndex"] = to_string(previous_index);
  GenericResponse* const gen_resp(new GenericResponse);
  Generic(key, params, EVHTTP_REQ_PUT, false, gen_resp,
          new Task(bind(&UpdateRequestDone, gen_resp, cb, _1),
                   event_base_.get()));
}


void EtcdClient::UpdateWithTTL(const string& key, const string& value,
                               const seconds& ttl,
                               const int64_t previous_index,
                               const UpdateCallback& cb) {
  map<string, string> params;
  params["value"] = value;
  params["prevIndex"] = to_string(previous_index);
  params["ttl"] = to_string(ttl.count());
  GenericResponse* const gen_resp(new GenericResponse);
  Generic(key, params, EVHTTP_REQ_PUT, false, gen_resp,
          new Task(bind(&UpdateRequestDone, gen_resp, cb, _1),
                   event_base_.get()));
}


void EtcdClient::ForceSet(const string& key, const string& value,
                          const ForceSetCallback& cb) {
  map<string, string> params;
  params["value"] = value;
  GenericResponse* const gen_resp(new GenericResponse);
  Generic(key, params, EVHTTP_REQ_PUT, false, gen_resp,
          new Task(bind(&ForceSetRequestDone, gen_resp, cb, _1),
                   event_base_.get()));
}


void EtcdClient::ForceSetWithTTL(const string& key, const string& value,
                                 const seconds& ttl,
                                 const ForceSetCallback& cb) {
  map<string, string> params;
  params["value"] = value;
  params["ttl"] = to_string(ttl.count());
  GenericResponse* const gen_resp(new GenericResponse);
  Generic(key, params, EVHTTP_REQ_PUT, false, gen_resp,
          new Task(bind(&ForceSetRequestDone, gen_resp, cb, _1),
                   event_base_.get()));
}


void EtcdClient::Delete(const string& key, const int64_t current_index,
                        const DeleteCallback& cb) {
  map<string, string> params;
  params["prevIndex"] = to_string(current_index);
  GenericResponse* const gen_resp(new GenericResponse);
  Generic(key, params, EVHTTP_REQ_DELETE, false, gen_resp,
          new Task(bind(&DeleteRequestDone, gen_resp, cb, _1),
                   event_base_.get()));
}


void EtcdClient::Watch(const string& key, const WatchCallback& cb,
                       Task* task) {
  VLOG(1) << "EtcdClient::Watch: " << key;

  // Hold the task at least until we add "state" to DeleteWhenDone.
  TaskHold hold(task);
  unique_ptr<WatchState> state(new WatchState(this, key, cb, task));
  task->DeleteWhenDone(state.release());
}


void EtcdClient::Generic(const string& key, const map<string, string>& params,
                         evhttp_cmd_type verb, bool separate_conn,
                         GenericResponse* resp, Task* task) {
  map<string, string> modified_params(params);
  if (FLAGS_etcd_consistent) {
    modified_params["consistent"] = "true";
  } else {
    LOG_EVERY_N(WARNING, 100) << "Sending request without 'consistent=true'";
  }
  if (FLAGS_etcd_quorum) {
    // "wait" and "quorum" appear to be incompatible.
    if (modified_params.find("wait") == modified_params.end()) {
      modified_params["quorum"] = "true";
    }
  } else {
    LOG_EVERY_N(WARNING, 100) << "Sending request without 'quorum=true'";
  }

  Request* const etcd_req(new Request(this, verb, key, separate_conn,
                                      modified_params, resp, task));
  task->DeleteWhenDone(etcd_req);

  // Issue the new request from the libevent dispatch loop. This is
  // not usually necessary, but in error cases, libevent can call us
  // back synchronously, and we want to avoid overflowing the stack in
  // case of repeated errors.
  event_base_->Add(bind(&Request::Run, etcd_req, GetLeader()));
}


}  // namespace cert_trans
