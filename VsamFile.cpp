/*
 * Licensed Materials - Property of IBM
 * (C) Copyright IBM Corp. 2017. All Rights Reserved.
 * US Government Users Restricted Rights - Use, duplication or disclosure restricted by GSA ADP Schedule Contract with IBM Corp.
*/
#include "VsamFile.h"
#include <node_buffer.h>
#include <unistd.h>
#include <dynit.h>
#include <sstream>
#include <numeric>

using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Isolate;
using v8::Local;
using v8::Number;
using v8::Object;
using v8::Persistent;
using v8::String;
using v8::Value;
using v8::Exception;
using v8::HandleScope;
using v8::Handle;
using v8::Array;
using v8::Integer;
using v8::Boolean;
using v8::MaybeLocal;

Persistent<Function> VsamFile::OpenSyncConstructor;
Persistent<Function> VsamFile::AllocSyncConstructor;

static const char* hexstrToBuffer (char* hexbuf, int buflen, const char* hexstr);
static const char* bufferToHexstr (char* hexstr, const char* hexbuf, const int hexbuflen);

static void print_amrc() {
  __amrc_type currErr = *__amrc; /* copy contents of __amrc */
  /* structure so that values */
#pragma convert("IBM-1047")
  printf("R15 value = %d\n", currErr.__code.__feedback.__rc);
  printf("Reason code = %d\n", currErr.__code.__feedback.__fdbk);
  printf("RBA = %d\n", currErr.__RBA);
  printf("Last op = %d\n", currErr.__last_op);
#pragma convert(pop)
}


void VsamFile::DeleteCallback(uv_work_t* req, int status) {
  VsamFile* obj = (VsamFile*)(req->data);
  delete req;
  //TODO: what if the write failed (buf != NULL)

  if (status == UV_ECANCELED)
    return;

  const unsigned argc = 1;
  HandleScope scope(obj->isolate_);
  Local<Value> argv[argc];
  if (obj->lastrc_ != 0) {
    argv[0] = Exception::TypeError(
                String::NewFromUtf8(obj->isolate_, "Failed to delete"));
    obj->lastrc_ = 0;
  }
  else
    argv[0] = v8::Null(obj->isolate_);
  auto fn = Local<Function>::New(obj->isolate_, obj->cb_);
  fn->Call(Null(obj->isolate_), argc, argv);
}


void VsamFile::WriteCallback(uv_work_t* req, int status) {
  VsamFile* obj = (VsamFile*)(req->data);
  delete req;

  if (status == UV_ECANCELED)
    return;

  const unsigned argc = 1;
  HandleScope scope(obj->isolate_);
  Local<Value> argv[argc];
  if (obj->lastrc_ != obj->reclen_)
    argv[0] = Exception::TypeError(
                String::NewFromUtf8(obj->isolate_, "Failed to write"));
  else
    argv[0] = v8::Null(obj->isolate_);
  auto fn = Local<Function>::New(obj->isolate_, obj->cb_);
  fn->Call(Null(obj->isolate_), argc, argv);
}


void VsamFile::UpdateCallback(uv_work_t* req, int status) {
  VsamFile* obj = (VsamFile*)(req->data);
  delete req;
  //TODO: what if the update failed (buf != NULL)

  if (status == UV_ECANCELED)
    return;

  const unsigned argc = 1;
  HandleScope scope(obj->isolate_);
  Local<Value> argv[argc] = { v8::Null(obj->isolate_) };
  auto fn = Local<Function>::New(obj->isolate_, obj->cb_);
  fn->Call(Null(obj->isolate_), argc, argv);
}


void VsamFile::ReadCallback(uv_work_t* req, int status) {
  VsamFile* obj = (VsamFile*)(req->data);
  delete req;

  if (status == UV_ECANCELED)
    return;

  char* buf = (char*)(obj->buf_);

  if (buf != NULL) {
    const unsigned argc = 2;
    HandleScope scope(obj->isolate_);
    Local<Object> record = Object::New(obj->isolate_);
    for(auto i = obj->layout_.begin(); i != obj->layout_.end(); ++i) {
      if (i->type == LayoutItem::STRING) { 
        std::string str; 
        transform(buf, buf + i->maxLength, back_inserter(str), [](char c) -> char {
          __e2a_l(&c, 1);
          return c;
        });
        record->Set(String::NewFromUtf8(obj->isolate_, &(i->name[0])),
                //node::Buffer::Copy(obj->isolate_, key.data(), key.size()).ToLocalChecked());
                String::NewFromUtf8(obj->isolate_, str.c_str()));
      }
      else if (i->type == LayoutItem::HEXADECIMAL) { 
        char hexstr[(i->maxLength*2)+1];
        bufferToHexstr(hexstr, buf, i->maxLength);
        std::string str(hexstr); 
        record->Set(String::NewFromUtf8(obj->isolate_, &(i->name[0])),
                String::NewFromUtf8(obj->isolate_, str.c_str()));
      }
      buf += i->maxLength;
    }
    Local<Value> argv[argc] = { record, v8::Null(obj->isolate_) };

    auto fn = Local<Function>::New(obj->isolate_, obj->cb_);
    fn->Call(Null(obj->isolate_), argc, argv);
  }
  else {
    const unsigned argc = 2;
    HandleScope scope(obj->isolate_);
    Local<Value> argv[argc] = { v8::Null(obj->isolate_),
                                v8::Null(obj->isolate_) };
    auto fn = Local<Function>::New(obj->isolate_, obj->cb_);
    fn->Call(Null(obj->isolate_), argc, argv);
  }
}


void VsamFile::Find(uv_work_t* req) {
  VsamFile* obj = (VsamFile*)(req->data);
  int rc;
  const char* buf;
  int buflen;
  if (obj->keybuf_) {
    if (obj->keybuf_len_==0) {
      Exception::TypeError(String::NewFromUtf8(obj->isolate_,
                           "find: Buffer object is empty."));
    }
    buf = obj->keybuf_;
    buflen = obj->keybuf_len_;
  } else {
    LayoutItem& key_layout = obj->layout_[obj->key_i_];
    if (key_layout.type == LayoutItem::HEXADECIMAL) {
      char buf[key_layout.maxLength];
      hexstrToBuffer(buf, sizeof(buf), obj->key_.c_str());
      rc = flocate(obj->stream_, buf, obj->keylen_, obj->equality_);
      goto chk;
    } else {
      buf = obj->key_.c_str();
      buflen = obj->keylen_;
    }
  }
  rc = flocate(obj->stream_, buf, buflen, obj->equality_);

chk: 
  if (rc==0) {
    char buf[obj->reclen_];
    int ret = fread(buf, obj->reclen_, 1, obj->stream_);
    //TODO: if read fails
    if (ret == 1) {
      obj->buf_ = malloc(obj->reclen_);
      //TODO: if malloc fails
      memcpy(obj->buf_, buf, obj->reclen_);
      return;
    }
  }
  if (obj->buf_) {
    free(obj->buf_);
    obj->buf_ = NULL;
  }
  if (obj->keybuf_) {
    free(obj->keybuf_);
    obj->keybuf_ = NULL;
  }
}

void VsamFile::Read(uv_work_t* req) {
  VsamFile* obj = (VsamFile*)(req->data);
  char buf[obj->reclen_];
  int ret = fread(buf, obj->reclen_, 1, obj->stream_);
  //TODO: if read fails
  if (ret == 1) {
    obj->buf_ = malloc(obj->reclen_);
    //TODO: if malloc fails
    memcpy(obj->buf_, buf, obj->reclen_);
    return;
  }
  if (obj->buf_) {
    free(obj->buf_);
    obj->buf_ = NULL;
  }
  if (obj->keybuf_) {
    free(obj->keybuf_);
    obj->keybuf_ = NULL;
  }
}


void VsamFile::Delete(uv_work_t* req) {
  VsamFile* obj = (VsamFile*)(req->data);
  obj->lastrc_ = fdelrec(obj->stream_);
}


void VsamFile::Write(uv_work_t* req) {
  VsamFile* obj = (VsamFile*)(req->data);
  obj->lastrc_ = fwrite(obj->buf_, 1, obj->reclen_, obj->stream_);
  free(obj->buf_);
  obj->buf_ = NULL;
  if (obj->keybuf_) {
    free(obj->keybuf_);
    obj->keybuf_ = NULL;
  }
}


void VsamFile::Update(uv_work_t* req) {
  VsamFile* obj = (VsamFile*)(req->data);
  int ret = fupdate(obj->buf_, obj->reclen_, obj->stream_);
  if (ret == 0) {
    //TODO: error
  }
  free(obj->buf_);
  obj->buf_ = NULL;
  if (obj->keybuf_) {
    free(obj->keybuf_);
    obj->keybuf_ = NULL;
  }
}


void VsamFile::Dealloc(uv_work_t* req) {
  VsamFile* obj = (VsamFile*)(req->data);

#pragma convert("IBM-1047")
  std::ostringstream dataset;
  dataset << "//'" << obj->path_.c_str() << "'";
#pragma convert(pop)

  obj->lastrc_ = remove(dataset.str().c_str());
}


void VsamFile::DeallocCallback(uv_work_t* req, int status) {
  VsamFile* obj = (VsamFile*)(req->data);
  delete req;

  HandleScope scope(obj->isolate_);
  if (status == UV_ECANCELED) {
    return;
  }
  else if (obj->lastrc_ != 0) {
    const unsigned argc = 1;
    Local<Value> argv[argc] = {
      Exception::TypeError(String::NewFromUtf8(obj->isolate_,
                           "Couldn't deallocate dataset"))
    };

    auto fn = Local<Function>::New(obj->isolate_, obj->cb_);
    fn->Call(Null(obj->isolate_), argc, argv);
  }
  else {
    const unsigned argc = 1;
    Local<Value> argv[argc] = {
      Null(obj->isolate_)
    };

    auto fn = Local<Function>::New(obj->isolate_, obj->cb_);
    fn->Call(Null(obj->isolate_), argc, argv);
  }
}


VsamFile::VsamFile(std::string& path, std::vector<LayoutItem>& layout,
                   Isolate* isolate, bool alloc, int key_i ) :
    path_(path),
    stream_(NULL),
    keylen_(-1),
    lastrc_(0),
    layout_(layout),
    key_i_(key_i),
    isolate_(isolate),
    buf_(NULL),
    keybuf_(NULL),
    keybuf_len_(0) {

#pragma convert("IBM-1047")
    std::ostringstream dataset;
    dataset << "//'" << path.c_str() << "'";
    stream_ = fopen(dataset.str().c_str(), "rb+,type=record");
    int err = __errno2();
#pragma convert(pop)

  if (!alloc) {

    if (stream_ == NULL) {
      errmsg_ = err == 0xC00B0641 ? "Dataset does not exist" : "Invalid dataset name";
      lastrc_ = -1;
      return;
    }

#pragma convert("IBM-1047")
    stream_ = freopen(dataset.str().c_str(), "ab+,type=record", stream_);
#pragma convert(pop)
    if (stream_ == NULL) {
      errmsg_ = "Failed to open dataset";
      lastrc_ = -1;
      return;
    }

  } else {
    
    if (stream_ != NULL) {
      errmsg_ = "Dataset already exists";
      fclose(stream_);
      lastrc_ = -1;
      return;
    }

    if (err != 0xC00B0641) {
      errmsg_ = "Invalid dataset format";
      lastrc_ = -1;
      return;
    }

    std::ostringstream ddname;
#pragma convert("IBM-1047")
    ddname << "NAMEDD";
#pragma convert(pop)

    __dyn_t dyn;
    dyninit(&dyn);
    dyn.__dsname = &(path_[0]);
    dyn.__ddname = &(ddname.str()[0]);
    dyn.__normdisp = __DISP_CATLG;
    dyn.__lrecl = std::accumulate(layout_.begin(), layout_.end(), 0,
                                  [](int n, LayoutItem& l) -> int { return n + l.maxLength; });
    dyn.__keylength = layout_[0].maxLength;
    dyn.__recorg = __KS;
    if (dynalloc(&dyn) != 0) {
      errmsg_ = "Failed to allocate dataset";
      lastrc_ = -1;
      return;
    }

#pragma convert("IBM-1047")
    stream_ = fopen(dataset.str().c_str(), "ab+,type=record");
#pragma convert(pop)
    if (stream_ == NULL) {
      errmsg_ = "Failed to open new dataset";
      lastrc_ = -1;
      return;
    }
  }

  fldata_t info;
  fldata(stream_, NULL, &info);
  keylen_ = info.__vsamkeylen;
  reclen_ = info.__maxreclen;
  if (keylen_ != layout_[0].maxLength) {
    errmsg_ = "Incorrect key length";
    fclose(stream_);
    lastrc_ = -1;
    return;
  }

}


VsamFile::~VsamFile() {
  if (stream_ != NULL)
    fclose(stream_);
}


void VsamFile::SetPrototypeMethods(Local<FunctionTemplate>& tpl) {
  // Prototype
  NODE_SET_PROTOTYPE_METHOD(tpl, "read", Read);
  NODE_SET_PROTOTYPE_METHOD(tpl, "find", FindEq);
  NODE_SET_PROTOTYPE_METHOD(tpl, "findeq", FindEq);
  NODE_SET_PROTOTYPE_METHOD(tpl, "findge", FindGe);
  NODE_SET_PROTOTYPE_METHOD(tpl, "findfirst", FindFirst);
  NODE_SET_PROTOTYPE_METHOD(tpl, "findlast", FindLast);
  NODE_SET_PROTOTYPE_METHOD(tpl, "update", Update);
  NODE_SET_PROTOTYPE_METHOD(tpl, "write", Write);
  NODE_SET_PROTOTYPE_METHOD(tpl, "delete", Delete);
  NODE_SET_PROTOTYPE_METHOD(tpl, "close", Close);
  NODE_SET_PROTOTYPE_METHOD(tpl, "dealloc", Dealloc);
}


void VsamFile::Init(Isolate* isolate) {
  // Prepare constructor template
  Local<FunctionTemplate> tpl = FunctionTemplate::New(isolate, VsamFile::OpenSync);
  tpl->SetClassName(String::NewFromUtf8(isolate, "VsamFile"));
  tpl->InstanceTemplate()->SetInternalFieldCount(1);
  SetPrototypeMethods(tpl);
  OpenSyncConstructor.Reset(isolate, tpl->GetFunction());

  tpl = FunctionTemplate::New(isolate, VsamFile::AllocSync);
  tpl->SetClassName(String::NewFromUtf8(isolate, "VsamFile"));
  tpl->InstanceTemplate()->SetInternalFieldCount(1);
  SetPrototypeMethods(tpl);
  AllocSyncConstructor.Reset(isolate, tpl->GetFunction());
}


void VsamFile::Construct(const FunctionCallbackInfo<Value>& args, bool alloc) {
  Isolate* isolate = args.GetIsolate();

  // Check the number of arguments passed.
  if (args.Length() != 2) {
    // Throw an Error that is passed back to JavaScript
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong number of arguments")));
    return;
  }

  if (!args[0]->IsString()) {
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong arguments")));
    return;
  }

  std::string path (*v8::String::Utf8Value(args[0]->ToString()));
  transform(path.begin(), path.end(), path.begin(), [](char c) -> char {
    __a2e_l(&c, 1);
    return c;
  });

  Local<Object> schema = args[1]->ToObject();
  Local<Array> properties = schema->GetPropertyNames();
  std::vector<LayoutItem> layout;
  int key_i = 0; // for its data type - default to first field if no "key" found
  for (int i = 0; i < properties->Length(); ++i) {
    String::Utf8Value name(properties->Get(i)->ToString());
 
    Local<Object> item = Local<Object>::Cast(schema->Get(properties->Get(i)));
    if (item.IsEmpty()) {
      isolate->ThrowException(Exception::TypeError( String::NewFromUtf8(isolate, "Json is incorrect")));
      return;
    }

    Local<Value> length = Local<Value>::Cast(item->Get( Local<String>(String::NewFromUtf8(isolate, "maxLength"))));
    if (length.IsEmpty() || !length->IsNumber()) {
      isolate->ThrowException(Exception::TypeError( String::NewFromUtf8(isolate, "Json is incorrect")));
      return;
    }

    Local<Value> jtype = item->Get( Local<String>(String::NewFromUtf8(isolate, "type")));
    if (jtype.IsEmpty()) {
      isolate->ThrowException(Exception::TypeError( String::NewFromUtf8(isolate, "Json \"type\" is empty")));
      return;
    }
    std::string stype(*v8::String::Utf8Value(jtype));
    if (!strcmp(stype.c_str(),"string")) {
      layout.push_back(LayoutItem(name, length->ToInteger()->Value(), LayoutItem::STRING));
    } else if (!strcmp(stype.c_str(),"hexadecimal")) {
      layout.push_back(LayoutItem(name, length->ToInteger()->Value(), LayoutItem::HEXADECIMAL));
    } else {
      isolate->ThrowException(Exception::TypeError( String::NewFromUtf8(isolate, "Json \"type\" must be \"string\" or \"hexadecimal\"")));
      return;
    }
    if (!strcmp(*name,"key")) {
      key_i = i;
    }
  }

  VsamFile *obj = new VsamFile(path, layout, isolate, alloc, key_i);
  if (obj->lastrc_) {
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, obj->errmsg_.c_str())));
    delete obj;
    return;
  }
  obj->Wrap(args.This());
  args.GetReturnValue().Set(args.This());
}


void VsamFile::AllocSync(const FunctionCallbackInfo<Value>& args) {
  if (args.IsConstructCall()) {
    Construct(args, true);
  } else {
    // Invoked as plain function `MyObject(...)`, turn into construct call.
    Isolate* isolate = args.GetIsolate();
    const int argc = 2;
    Local<Value> argv[argc] = { args[0], args[1] };
    Local<Function> cons = Local<Function>::New(isolate, AllocSyncConstructor);
    Local<Context> context = isolate->GetCurrentContext();
    MaybeLocal<Object> instance =
        cons->NewInstance(context, argc, argv);
    if(!instance.IsEmpty())
      args.GetReturnValue().Set(instance.ToLocalChecked());
  }
}


void VsamFile::OpenSync(const FunctionCallbackInfo<Value>& args) {
  if (args.IsConstructCall()) {
    Construct(args, false);
  } else {
    // Invoked as plain function `MyObject(...)`, turn into construct call.
    Isolate* isolate = args.GetIsolate();
    const int argc = 2;
    Local<Value> argv[argc] = { args[0], args[1] };
    Local<Function> cons = Local<Function>::New(isolate, OpenSyncConstructor);
    Local<Context> context = isolate->GetCurrentContext();
    MaybeLocal<Object> instance =
        cons->NewInstance(context, argc, argv);
    if(!instance.IsEmpty())
      args.GetReturnValue().Set(instance.ToLocalChecked());
  }
}


void VsamFile::Exist(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  if (args.Length() != 1) {
    // Throw an Error that is passed back to JavaScript
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong number of arguments")));
    return;
  }

  if (!args[0]->IsString()) {
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong arguments")));
    return;
  }

  std::string path (*v8::String::Utf8Value(args[0]->ToString()));
  transform(path.begin(), path.end(), path.begin(), [](char c) -> char {
    __a2e_l(&c, 1);
    return c;
  });

#pragma convert("IBM-1047")
    std::ostringstream dataset;
    dataset << "//'" << path.c_str() << "'";
    FILE *stream = fopen(dataset.str().c_str(), "rb+,type=record");
#pragma convert(pop)

    if (stream == NULL) {
      args.GetReturnValue().Set(Boolean::New(isolate, false));
      return;
    }

    fclose(stream);
    args.GetReturnValue().Set(Boolean::New(isolate, true));
}


void VsamFile::Close(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  VsamFile* obj = ObjectWrap::Unwrap<VsamFile>(args.Holder());
  if (obj->stream_ == NULL) {
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "VSAM file is not open")));
    return;
  }

  if (fclose(obj->stream_)) {
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Error closing file")));
    return;
  }
  obj->stream_ = NULL;
}


void VsamFile::Delete(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  if (args.Length() < 1) {
    // Throw an Error that is passed back to JavaScript
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong number of arguments")));
    return;
  }

  if (!args[0]->IsFunction()) {
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong arguments")));
    return;
  }

  VsamFile* obj = ObjectWrap::Unwrap<VsamFile>(args.Holder());
  uv_work_t* request = new uv_work_t;
  request->data = obj;

  obj->cb_ = Persistent<Function>(isolate, Handle<Function>::Cast(args[0]));
  obj->isolate_ = isolate;

  uv_queue_work(uv_default_loop(), request, Delete, DeleteCallback);
}


void VsamFile::Write(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  if (args.Length() < 2) {
    // Throw an Error that is passed back to JavaScript
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong number of arguments")));
    return;
  }

  if (!args[1]->IsFunction()) {
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong arguments")));
    return;
  }

  Local<Object> record = args[0]->ToObject();
  VsamFile* obj = ObjectWrap::Unwrap<VsamFile>(args.Holder());
  obj->buf_ = malloc(obj->reclen_); //TODO: error
  memset(obj->buf_,0,obj->reclen_);
  char* buf = (char*)obj->buf_;
  for(auto i = obj->layout_.begin(); i != obj->layout_.end(); ++i) {
    Local<String> field = Local<String>::Cast(record->Get(String::NewFromUtf8(obj->isolate_, &(i->name[0]))));
    if (!field->IsString()) {
      isolate->ThrowException(Exception::TypeError(
          String::NewFromUtf8(isolate, "Currently only string (including hexadecimal string) is allowed as a field value.")));
      return;
    }
    if (i->type == LayoutItem::STRING || i->type == LayoutItem::HEXADECIMAL) {
      std::string key (*v8::String::Utf8Value(field));
      transform(key.begin(), key.end(), key.begin(), [](char c) -> char {
        __a2e_l(&c, 1);
        return c;
      });
      if (i->type == LayoutItem::STRING) {
        memcpy(buf, key.c_str(), key.length() + 1);
      } else {
        hexstrToBuffer(buf, i->maxLength, key.c_str());
      }
    } else {
      isolate->ThrowException(Exception::TypeError(
          String::NewFromUtf8(isolate, "Unexpected JSON data type")));
      return;
    }
    buf += i->maxLength;
  }

  uv_work_t* request = new uv_work_t;
  request->data = obj;
  obj->isolate_ = isolate;
  uv_queue_work(uv_default_loop(), request, Write, WriteCallback);
  obj->cb_ = Persistent<Function>(isolate, Handle<Function>::Cast(args[1]));
}


void VsamFile::Update(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  if (args.Length() < 2) {
    // Throw an Error that is passed back to JavaScript
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong number of arguments")));
    return;
  }

  if (!args[1]->IsFunction()) {
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong arguments")));
    return;
  }

  Local<Object> record = args[0]->ToObject();
  VsamFile* obj = ObjectWrap::Unwrap<VsamFile>(args.Holder());
  obj->buf_ = malloc(obj->reclen_); //TODO: error
  char* buf = (char*)obj->buf_;
  for(auto i = obj->layout_.begin(); i != obj->layout_.end(); ++i) {
    Local<String> field = Local<String>::Cast(record->Get(String::NewFromUtf8(obj->isolate_, &(i->name[0]))));
    if (!field->IsString()) {
      isolate->ThrowException(Exception::TypeError(
          String::NewFromUtf8(isolate, "Currently only string (including hexadecimal string) is allowed as a field value.")));
      return;
    }
    if (i->type == LayoutItem::STRING || i->type == LayoutItem::HEXADECIMAL) {
      std::string key (*v8::String::Utf8Value(field));
      transform(key.begin(), key.end(), key.begin(), [](char c) -> char {
        __a2e_l(&c, 1);
        return c;
      });
      if (i->type == LayoutItem::STRING) {
        memcpy(buf, key.c_str(), key.length() + 1);
      } else {
        hexstrToBuffer(buf, i->maxLength, key.c_str());
      }
      buf += i->maxLength;

    } else {
      isolate->ThrowException(Exception::TypeError(
          String::NewFromUtf8(isolate, "Unexpected JSON data type")));
      return;
    }
  }

  uv_work_t* request = new uv_work_t;
  request->data = obj;

  obj->cb_ = Persistent<Function>(isolate, Handle<Function>::Cast(args[1]));
  obj->isolate_ = isolate;

  uv_queue_work(uv_default_loop(), request, Update, UpdateCallback);
}

void VsamFile::FindEq(const FunctionCallbackInfo<Value>& args) {
  Find(args, __KEY_EQ);
}

void VsamFile::FindGe(const FunctionCallbackInfo<Value>& args) {
  Find(args, __KEY_GE);
}

void VsamFile::FindFirst(const FunctionCallbackInfo<Value>& args) {
  Find(args, __KEY_FIRST);
}

void VsamFile::FindLast(const FunctionCallbackInfo<Value>& args) {
  Find(args, __KEY_LAST);
}

void VsamFile::Find(const FunctionCallbackInfo<Value>& args, int equality) {
  Isolate* isolate = args.GetIsolate();

  std::string key = "";
  int callbackArg = 0;
  char* keybuf = NULL;
  int keybuf_len = 0;

  if (equality != __KEY_LAST && equality != __KEY_FIRST)  {
    if (args.Length() < 2) {
      // Throw an Error that is passed back to JavaScript
      isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong number of arguments")));
      return;
    }

    if (args[0]->IsString()) {
      key = std::string(*v8::String::Utf8Value(args[0]->ToString()));
      transform(key.begin(), key.end(), key.begin(), [](char c) -> char {
        __a2e_l(&c, 1);
        return c;
      });
      callbackArg = 1;
    } else if (args[0]->IsObject()) {
      char* buf = node::Buffer::Data(args[0]->ToObject());
      if (!args[1]->IsUint32()) {
        isolate->ThrowException(Exception::TypeError(
          String::NewFromUtf8(isolate, "Buffer argument must be followed by its length.")));
        return;
      }
      keybuf_len = args[1]->Uint32Value();
      if (keybuf_len > 0) {
        keybuf = (char*)malloc(keybuf_len); // TODO check error
        memcpy(keybuf, buf, keybuf_len);
      }
      callbackArg = 2;
    } else {
      isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "First argument must be either a string or a Buffer object.")));
      return;
    } 
    if (!args[callbackArg]->IsFunction()) {
      char err[128];
      if (callbackArg==1) {
        strcpy(err,"Second argument must be a function.");
      } else if (callbackArg==2) {
        strcpy(err,"Thrid argument must be a function.");
      }
      isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, err)));
      return;
    }

  } else {
    if (args.Length() < 1) {
      // Throw an Error that is passed back to JavaScript
      isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong number of arguments. 1 argument expected.")));
      return;
    }
    if (!args[0]->IsFunction()) {
      isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "First argument must be a function")));
      return;
    }
  }

  VsamFile* obj = ObjectWrap::Unwrap<VsamFile>(args.Holder());
  uv_work_t* request = new uv_work_t;
  request->data = obj;

  obj->cb_ = Persistent<Function>(isolate, Handle<Function>::Cast(args[callbackArg]));
  obj->isolate_ = isolate;
  if (!keybuf)
    obj->key_ = key;
  else {
    obj->keybuf_ = keybuf;
    obj->keybuf_len_ = keybuf_len;
  }
  obj->equality_ = equality;

  uv_queue_work(uv_default_loop(), request, Find, ReadCallback);
}

void VsamFile::Read(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  if (args.Length() < 1) {
    // Throw an Error that is passed back to JavaScript
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong number of arguments")));
    return;
  }

  if (!args[0]->IsFunction()) {
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong arguments")));
    return;
  }

  VsamFile* obj = ObjectWrap::Unwrap<VsamFile>(args.Holder());
  uv_work_t* request = new uv_work_t;
  request->data = obj;

  obj->cb_ = Persistent<Function>(isolate, Handle<Function>::Cast(args[0]));
  obj->isolate_ = isolate;

  uv_queue_work(uv_default_loop(), request, Read, ReadCallback);
}


void VsamFile::Dealloc(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  if (args.Length() < 1) {
    // Throw an Error that is passed back to JavaScript
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong number of arguments")));
    return;
  }

  if (!args[0]->IsFunction()) {
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Wrong arguments")));
    return;
  }

  VsamFile* obj = ObjectWrap::Unwrap<VsamFile>(args.Holder());
  if (obj->stream_ != NULL) {
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8(isolate, "Cannot dealloc an open VSAM file")));
    return;
  }
  uv_work_t* request = new uv_work_t;
  request->data = obj;

  obj->cb_ = Persistent<Function>(isolate, Handle<Function>::Cast(args[0]));
  obj->isolate_ = isolate;

  uv_queue_work(uv_default_loop(), request, Dealloc, DeallocCallback);
}

static const char* hexstrToBuffer (char* hexbuf, int buflen, const char* hexstr) {
   const int hexstrlen = strlen(hexstr);
   memset(hexbuf,0,buflen);
   char xx[2];
   int i, j, x;
   for (i=0,j=0; i<hexstrlen-(hexstrlen%2); ) {
     xx[0] = hexstr[i++];
     xx[1] = hexstr[i++];
#pragma convert("IBM-1047")
     sscanf(xx,"%2x", &x);
#pragma convert(pop)
     hexbuf[j++] = x;
   }
   if (hexstrlen%2) {
     xx[0] = hexstr[i];
#pragma convert("IBM-1047")
     xx[1] = '0';
     sscanf(xx,"%2x", &x);
#pragma convert(pop)
     hexbuf[j] = x;
   }
   return hexbuf;
}

static const char* bufferToHexstr (char* hexstr, const char* hexbuf, const int hexbuflen) {
   int i, j;
   for (i=0,j=0; i<hexbuflen; i++,j+=2) {
     if (hexbuf[i]==0) {
#pragma convert("IBM-1047")
       memset(hexstr+j,'0',2);
#pragma convert(pop)
     } else {
#pragma convert("IBM-1047")
       sprintf(hexstr+j,"%02x", hexbuf[i]);
#pragma convert(pop)
     }
   }
#pragma convert("IBM-1047")
   //remove trailing '0's, unless value is '00'
   while(--j>2 && hexstr[j]=='0')
     ;
#pragma convert(pop)
   hexstr[++j] = 0;
   __e2a_l(hexstr,strlen(hexstr));
   return hexstr;
}
