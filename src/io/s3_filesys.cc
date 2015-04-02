#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <ctime>
#include <sstream>
extern "C" {
#include <errno.h>
#include <curl/curl.h>
#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/md5.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
}
#include <dmlc/io.h>
#include <dmlc/logging.h>
#include "./s3_filesys.h"

namespace dmlc {
namespace io {
/*! \brief namespace for helper utils */
namespace s3 {
// simple XML parser
struct XMLIter {
  // content of xml
  const char *content_;
  // end of content
  const char *cend_;
  explicit XMLIter() 
      : content_(NULL), cend_(NULL) {
  }
  // constructor
  explicit XMLIter(const char *content)
      : content_(content) {
    cend_ = content_ + strlen(content_);
  }
  /*! \brief convert to string */
  inline std::string str(void) const {
    if (content_ >= cend_) return std::string("");
    return std::string(content_, cend_ - content_);
  }
  /*!
   * \brief get next value of corresponding key in xml string
   * \param key the key in xml field
   * \param value the return value if success
   * \return if the get is success
   */
  inline bool GetNext(const char *key,                          
                      XMLIter *value) {
    std::string begin = std::string("<") + key +">";
    std::string end = std::string("</") + key +">";
    const char *pbegin = strstr(content_, begin.c_str());
    if (pbegin == NULL || pbegin > cend_) return false;
    content_ = pbegin + begin.size();
    const char *pend = strstr(content_, end.c_str());
    CHECK(pend != NULL) << "bad xml format";
    value->content_ = content_;
    value->cend_ = pend;
    content_ = pend + end.size();
    return true;
  }
};
/*!
 * \brief return a base64 encodes string
 * \param md the data
 * \param len the length of data
 * \return the encoded string
 */
std::string Base64(unsigned char md[], unsigned len) {
  // encode base64
  BIO *fp = BIO_push(BIO_new(BIO_f_base64()),
                       BIO_new(BIO_s_mem()));    
  BIO_write(fp, md, len);
  BIO_ctrl(fp, BIO_CTRL_FLUSH, 0, NULL);    
  BUF_MEM *res;
  BIO_get_mem_ptr(fp, &res);   
  std::string ret(res->data, res->length - 1);
  BIO_free_all(fp);
  return ret;
}
/*!
 * \brief sign given AWS secret key
 * \param secret_key the key to compute the sign
 * \param content the content to sign
 */
std::string Sign(const std::string &key, const std::string &content) {
  HMAC_CTX ctx;
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned rlen = 0;
  HMAC_CTX_init(&ctx);
  HMAC_Init(&ctx, key.c_str(), key.length(), EVP_sha1());
  HMAC_Update(&ctx,
              reinterpret_cast<const unsigned char*>(content.c_str()),
                content.length());
  HMAC_Final(&ctx, md, &rlen);
  HMAC_CTX_cleanup(&ctx);
  return Base64(md, rlen);
}
// sign AWS key
std::string Sign(const std::string &key,
                 const std::string &method,
                 const std::string &content_md5,
                 const std::string &content_type,
                 const std::string &date,
                 std::vector<std::string> amz_headers,
                 const std::string &resource) {
  std::ostringstream stream;
  stream << method << "\n";
  stream << content_md5 << "\n";
  stream << content_type << "\n";
  stream << date << "\n";
  std::sort(amz_headers.begin(), amz_headers.end());
  for (size_t i = 0; i < amz_headers.size(); ++i) {
    stream << amz_headers[i] << "\n";
  }
  stream << resource;
  return Sign(key, stream.str());
}

std::string ComputeMD5(const std::string &buf) {
  if (buf.length() == 0) return "";
  const int kLen = 128 / 8;
  unsigned char md[kLen];
  MD5(reinterpret_cast<const unsigned char *>(buf.c_str()),
      buf.length(), md);
  return Base64(md, kLen);
}
// remove the beginning slash at name
inline const char *RemoveBeginSlash(const std::string &name) {
  const char *s = BeginPtr(name);
  while (*s == '/') {
    ++s;
  }
  return s;
}
// fin dthe error field of the header
inline bool FindHttpError(const std::string &header) {
  std::string hd, ret;
  int code;
  std::istringstream is(header);
  if (is >> hd >> code >> ret) {
    if (code == 206 || ret == "OK") {
      return false;
    } else if (ret == "Continue") {
      return false;
    }
  }
  return true;
}
/*!
 * \brief get the datestring needed by AWS
 * \return datestring
 */
inline std::string GetDateString(void) {
  time_t t = time(NULL);
  tm gmt;
  gmtime_r(&t, &gmt);
  char buf[256];
  strftime(buf, 256, "%a, %d %b %Y %H:%M:%S GMT", &gmt);
  return std::string(buf);
}
// curl callback to write sstream
size_t WriteSStreamCallback(char *buf, size_t size, size_t count, void *fp) {
  static_cast<std::ostringstream*>(fp)->write(buf, size * count);
  return size * count;
}
// callback by curl to write to std::string
size_t WriteStringCallback(char *buf, size_t size, size_t count, void *fp) {
  size *= count;
  std::string *str = static_cast<std::string*>(fp);
  size_t len = str->length();
  str->resize(len + size);
  std::memcpy(BeginPtr(*str) + len, buf, size);
  return size;
}

// useful callback for reading memory
struct ReadStringStream {
  const char *dptr;
  size_t nleft;
  // constructor
  ReadStringStream(const std::string &data) {
    dptr = BeginPtr(data);
    nleft = data.length();
  }
  // curl callback to write sstream
  static size_t Callback(char *buf, size_t size, size_t count, void *fp) {
    size *= count;
    ReadStringStream *s = static_cast<ReadStringStream*>(fp);
    size_t nread = std::min(size, s->nleft);
    std::memcpy(buf, s->dptr, nread);
    s->dptr += nread; s->nleft -= nread;
    return nread;
  }
};

/*! \brief reader stream that can be used to read */
class ReadStream : public ISeekStream {
 public:
  ReadStream(const URI &path,
             const std::string &aws_id,
             const std::string &aws_key)
      : path_(path), aws_id_(aws_id), aws_key_(aws_key),
        mcurl_(NULL), ecurl_(NULL), slist_(NULL),
        read_ptr_(0), curr_bytes_(0), at_end_(false) {
  }
  virtual ~ReadStream(void) {
    this->Cleanup();
  }
  virtual void Write(const void *ptr, size_t size) {
    CHECK(false) << "S3.ReadStream cannot be used for write";
  }
  // lazy seek function
  virtual void Seek(size_t pos) {
    if (curr_bytes_ != pos) {
      this->Cleanup();    
      curr_bytes_ = pos;
    }
  }
  virtual size_t Tell(void) {
    return curr_bytes_;
  }
  virtual bool AtEnd(void) const {
    return at_end_;
  }  
  virtual size_t Read(void *ptr, size_t size);
  /*!
   * \brief initialize the stream at begin_bytes
   * this is the true seek function
   * \param begin_bytes the beginning bytes of the stream
   */
  void Init(size_t begin_bytes);

 private:
  // path we are reading
  URI path_;
  // aws access key and id
  std::string aws_id_, aws_key_;
  // multi and easy curl handle
  CURL *mcurl_, *ecurl_;
  // slist needed by the program
  curl_slist *slist_;
  // data buffer
  std::string buffer_;
  // header buffer
  std::string header_;
  // data pointer to read position
  size_t read_ptr_;
  // current position in the stream
  size_t curr_bytes_;
  // mark end of stream
  bool at_end_;
  /*!
   * \brief initialize the stream at begin_bytes
   * \param begin_bytes the beginning bytes of the stream
   */
  void Cleanup(void);
  /*!
   * \brief try to fill the buffer with at least wanted bytes
   * \param want_bytes number of bytes we want to fill
   * \return number of remainning running curl handles
   */
  int FillBuffer(size_t want_bytes);
};

// initialize the reader at begin bytes
void ReadStream::Init(size_t begin_bytes) {
  CHECK(mcurl_ == NULL && ecurl_ == NULL &&
        slist_ == NULL) << "must call init in clean state";
  // initialize the curl request
  std::vector<std::string> amz;
  std::string date = GetDateString();
  std::string signature = Sign(aws_key_, "GET", "", "", date, amz,
                               std::string("/") + path_.host + '/' + RemoveBeginSlash(path_.name));
  // generate headers
  std::ostringstream sauth, sdate, surl, srange;
  std::ostringstream result;
  sauth << "Authorization: AWS " << aws_id_ << ":" << signature;
  sdate << "Date: " << date;
  surl << "http://" << path_.host << ".s3.amazonaws.com" << '/'
       << RemoveBeginSlash(path_.name);
  srange << "Range: bytes=" << begin_bytes << "-";
  // make request
  ecurl_ = curl_easy_init();
  slist_ = curl_slist_append(slist_, sdate.str().c_str());
  slist_ = curl_slist_append(slist_, srange.str().c_str());
  slist_ = curl_slist_append(slist_, sauth.str().c_str());
  CHECK(curl_easy_setopt(ecurl_, CURLOPT_HTTPHEADER, slist_) == CURLE_OK);
  CHECK(curl_easy_setopt(ecurl_, CURLOPT_URL, surl.str().c_str()) == CURLE_OK);
  CHECK(curl_easy_setopt(ecurl_, CURLOPT_HTTPGET, 1L) == CURLE_OK);
  CHECK(curl_easy_setopt(ecurl_, CURLOPT_HEADER, 0L) == CURLE_OK);
  CHECK(curl_easy_setopt(ecurl_, CURLOPT_WRITEFUNCTION, WriteStringCallback) == CURLE_OK);
  CHECK(curl_easy_setopt(ecurl_, CURLOPT_WRITEDATA, &buffer_) == CURLE_OK);
  CHECK(curl_easy_setopt(ecurl_, CURLOPT_HEADERFUNCTION, WriteStringCallback) == CURLE_OK);
  CHECK(curl_easy_setopt(ecurl_, CURLOPT_HEADERDATA, &header_) == CURLE_OK);
  mcurl_ = curl_multi_init();
  CHECK(curl_multi_add_handle(mcurl_, ecurl_) == CURLM_OK);
  int nrun;
  curl_multi_perform(mcurl_, &nrun);
  CHECK(nrun != 0 || header_.length() != 0 || buffer_.length() != 0);
  // start running and check header
  this->FillBuffer(1);
  if (FindHttpError(header_)) {
    while (this->FillBuffer(buffer_.length() + 256) != 0);
    LOG(FATAL) << "AWS S3 Error:\n" << header_ << buffer_;
  }
  // setup the variables
  at_end_ = false;
  curr_bytes_ = begin_bytes;
  read_ptr_ = 0;
}
// read data in
size_t ReadStream::Read(void *ptr, size_t size) {
  // lazy initialize
  if (mcurl_ == NULL) Init(curr_bytes_);
  // check at end
  if (at_end_) return 0;

  size_t nleft = size;
  char *buf = reinterpret_cast<char*>(ptr);
  while (nleft != 0) {
    if (read_ptr_ == buffer_.length()) {
      read_ptr_ = 0; buffer_.clear();
      if (this->FillBuffer(nleft) == 0 && buffer_.length() == 0) {
        at_end_ = true;
        break;
      }
    }
    size_t nread = std::min(nleft, buffer_.length() - read_ptr_);
    std::memcpy(buf, BeginPtr(buffer_) + read_ptr_, nread);
    buf += nread; read_ptr_ += nread; nleft -= nread;
  }
  size_t read_bytes = size - nleft;
  curr_bytes_ += read_bytes;
  return read_bytes;
}
// fill the buffer with wanted bytes
int ReadStream::FillBuffer(size_t nwant) {
  int nrun = 0;
  while (buffer_.length() < nwant) {
    // wait for the event of read ready
    fd_set fdread;
    fd_set fdwrite;
    fd_set fdexcep;
    FD_ZERO(&fdread);
    FD_ZERO(&fdwrite);
    FD_ZERO(&fdexcep);
    int maxfd = -1;
    timeval timeout;
    timeout.tv_sec = 60;
    timeout.tv_usec = 0;
    long curl_timeo;
    curl_multi_timeout(mcurl_, &curl_timeo);
    if (curl_timeo >= 0) {
      timeout.tv_sec = curl_timeo / 1000;
      if(timeout.tv_sec > 1) {
        timeout.tv_sec = 1;
      } else {
        timeout.tv_usec = (curl_timeo % 1000) * 1000;
      }
    }
    CHECK(curl_multi_fdset(mcurl_, &fdread, &fdwrite, &fdexcep, &maxfd) == CURLM_OK);
    int rc;
    if (maxfd == -1) {
#ifdef _WIN32
      Sleep(100);
      rc = 0;
#else
      struct timeval wait = { 0, 100 * 1000 };
      rc = select(0, NULL, NULL, NULL, &wait);
#endif
    } else {
      rc = select(maxfd + 1, &fdread, &fdwrite, &fdexcep, &timeout);
    }
    if (rc != -1) {
      CURLMcode ret = curl_multi_perform(mcurl_, &nrun);
      if (ret ==  CURLM_CALL_MULTI_PERFORM) continue;
      CHECK(ret == CURLM_OK);
      if (nrun == 0) return 0;
    }
  }
  return nrun;
}
// cleanup the previous sessions for restart
void ReadStream::Cleanup() {  
  if (mcurl_ != NULL) {
    curl_multi_remove_handle(mcurl_, ecurl_);
    curl_easy_cleanup(ecurl_);
    curl_multi_cleanup(mcurl_);
    curl_slist_free_all(slist_);
    mcurl_ = NULL;
    ecurl_ = NULL;
    slist_ = NULL;
  }
  buffer_.clear(); header_.clear();
  curr_bytes_ = 0; at_end_ = false;
}
// End of ReadStream functions
class WriteStream : public IStream {
 public:
  WriteStream(const URI &path,
              const std::string &aws_id,
              const std::string &aws_key)
      : path_(path), aws_id_(aws_id),
        aws_key_(aws_key) {
    const char *buz = getenv("DMLC_S3_WRITE_BUFFER_MB");
    if (buz != NULL) {
      max_buffer_size_ = static_cast<size_t>(atol(buz)) << 20UL;
    } else {
      // 64 MB
      const size_t kDefaultBufferSize = 64 << 20UL;
      max_buffer_size_ = kDefaultBufferSize;
    }
    ecurl_ = curl_easy_init();
    this->Init();
  }
  virtual size_t Read(void *ptr, size_t size) {
    CHECK(false) << "S3.WriteStream cannot be used for read";
    return 0;
  }
  virtual void Write(const void *ptr, size_t size);
  // destructor
  virtual ~WriteStream() {
    this->Upload();
    this->Finish();
    curl_easy_cleanup(ecurl_);
  }
    
 private:
  // internal maximum buffer size
  size_t max_buffer_size_;
  // path we are reading
  URI path_;
  // aws access key and id
  std::string aws_id_, aws_key_;
  // easy curl handle used for the request
  CURL *ecurl_;
  // upload_id used by AWS
  std::string upload_id_;
  // write data buffer
  std::string buffer_;
  // etags of each part we uploaded
  std::vector<std::string> etags_;
  // part id of each part we uploaded
  std::vector<size_t> part_ids_;
  /*!
   * \brief helper function to do http post request
   * \param method method to peform
   * \param path the resource to post
   * \param url_args additional arguments in URL
   * \param url_args translated arguments to sign
   * \param content_type content type of the data
   * \param data data to post
   * \param out_header holds output Header
   * \param out_data holds output data
   */
  void Run(const std::string &method,
           const URI &path,
           const std::string &args,
           const std::string &content_type,
           const std::string &data,
           std::string *out_header,
           std::string *out_data);
  /*!
   * \brief initialize the upload request
   */
  void Init(void);
  /*!
   * \brief upload the buffer to S3, store the etag
   * clear the buffer
   */
  void Upload(void);
  /*!
   * \brief commit the upload and finish the session
   */
  void Finish(void);
};

void WriteStream::Write(const void *ptr, size_t size) {
  size_t rlen = buffer_.length();
  buffer_.resize(rlen + size);
  std::memcpy(BeginPtr(buffer_) + rlen, ptr, size);
  if (buffer_.length() >= max_buffer_size_) {
    this->Upload();
  }
}

void WriteStream::Run(const std::string &method,
                      const URI &path,
                      const std::string &args,
                      const std::string &content_type,
                      const std::string &data,
                      std::string *out_header,
                      std::string *out_data) {
  // initialize the curl request
  std::vector<std::string> amz;
  std::string md5str = ComputeMD5(data);
  std::string date = GetDateString();
  std::string signature = Sign(aws_key_, method.c_str(), md5str,
                               content_type, date, amz,
                               std::string("/") + path_.host + '/' +
                               RemoveBeginSlash(path_.name) + args);
  
  // generate headers
  std::ostringstream sauth, sdate, surl, scontent, smd5;
  std::ostringstream rheader, rdata;
  sauth << "Authorization: AWS " << aws_id_ << ":" << signature;
  sdate << "Date: " << date;
  surl << "http://" << path_.host << ".s3.amazonaws.com" << '/'
       << RemoveBeginSlash(path_.name) << args;
  scontent << "Content-Type: " << content_type;
  // helper for read string
  ReadStringStream ss(data);
  // list
  curl_slist *slist = NULL;
  slist = curl_slist_append(slist, sdate.str().c_str());
  slist = curl_slist_append(slist, scontent.str().c_str());
  if (md5str.length() != 0) {
    smd5 << "Content-MD5: " << md5str;
    slist = curl_slist_append(slist, smd5.str().c_str());
  }
  slist = curl_slist_append(slist, sauth.str().c_str());
  curl_easy_reset(ecurl_);
  CHECK(curl_easy_setopt(ecurl_, CURLOPT_HTTPHEADER, slist) == CURLE_OK);
  CHECK(curl_easy_setopt(ecurl_, CURLOPT_URL, surl.str().c_str()) == CURLE_OK);
  CHECK(curl_easy_setopt(ecurl_, CURLOPT_HEADER, 0L) == CURLE_OK);
  CHECK(curl_easy_setopt(ecurl_, CURLOPT_WRITEFUNCTION, WriteSStreamCallback) == CURLE_OK);
  CHECK(curl_easy_setopt(ecurl_, CURLOPT_WRITEDATA, &rdata) == CURLE_OK);  
  CHECK(curl_easy_setopt(ecurl_, CURLOPT_WRITEHEADER, WriteSStreamCallback) == CURLE_OK);
  CHECK(curl_easy_setopt(ecurl_, CURLOPT_HEADERDATA, &rheader) == CURLE_OK);
  if (method == "POST") {
    CHECK(curl_easy_setopt(ecurl_, CURLOPT_POST, 0L) == CURLE_OK);
    CHECK(curl_easy_setopt(ecurl_, CURLOPT_POSTFIELDSIZE, data.length()) == CURLE_OK);
    CHECK(curl_easy_setopt(ecurl_, CURLOPT_POSTFIELDS, BeginPtr(data)) == CURLE_OK);
  } else if (method == "PUT") {
    CHECK(curl_easy_setopt(ecurl_, CURLOPT_PUT, 1L) == CURLE_OK);
    CHECK(curl_easy_setopt(ecurl_, CURLOPT_READDATA, &ss) == CURLE_OK);
    CHECK(curl_easy_setopt(ecurl_, CURLOPT_INFILESIZE_LARGE, data.length()) == CURLE_OK);
    CHECK(curl_easy_setopt(ecurl_, CURLOPT_READFUNCTION, ReadStringStream::Callback) == CURLE_OK);
  }
  CHECK(curl_easy_perform(ecurl_) == CURLE_OK);
  curl_slist_free_all(slist);
  *out_header = rheader.str();
  *out_data = rdata.str();
  if (FindHttpError(*out_header) ||
      out_data->find("<Error>") != std::string::npos) {
    LOG(FATAL) << "AWS S3 Error:\n" << *out_header << *out_data;
  }
}
void WriteStream::Init(void) {
  std::string rheader, rdata;
  Run("POST", path_, "?uploads",
      "binary/octel-stream", "", &rheader, &rdata);
  XMLIter xml(rdata.c_str());
  XMLIter upid;
  CHECK(xml.GetNext("UploadId", &upid)) << "missing UploadId";
  upload_id_ = upid.str();
}

void WriteStream::Upload(void) {
  if (buffer_.length() == 0) return;
  std::ostringstream sarg;
  std::string rheader, rdata;
  size_t partno = etags_.size() + 1;

  sarg << "?partNumber=" << partno << "&uploadId=" << upload_id_;
  Run("PUT", path_, sarg.str(),
      "binary/octel-stream", buffer_, &rheader, &rdata);
  const char *p = strstr(rheader.c_str(), "ETag: ");
  CHECK(p != NULL) << "cannot find ETag in header";
  p = strchr(p, '\"');
  CHECK(p != NULL) << "cannot find ETag in header";  
  const char *end = strchr(p + 1, '\"');
  CHECK(end != NULL) << "cannot find ETag in header";
  
  etags_.push_back(std::string(p, end - p + 1));
  part_ids_.push_back(partno);
  buffer_.clear();
}

void WriteStream::Finish(void) {
  std::ostringstream sarg, sdata;
  std::string rheader, rdata;
  sarg << "?uploadId=" << upload_id_;
  sdata << "<CompleteMultipartUpload>\n";
  CHECK(etags_.size() == part_ids_.size());
  for (size_t i = 0; i < etags_.size(); ++i) {
    sdata << " <Part>\n"
          << "  <PartNumber>" << part_ids_[i] << "</PartNumber>\n"
          << "  <ETag>" << etags_[i] << "</ETag>\n"
          << " </Part>\n";
  }
  sdata << "</CompleteMultipartUpload>\n";
  Run("POST", path_, sarg.str(),
      "text/xml", sdata.str(), &rheader, &rdata);
}
/*!
 * \brief list the objects in the bucket with prefix specified by path.name
 * \param path the path to query
 * \param aws_id access id of aws
 * \param aws_key access key of aws
 * \paam out_list stores the output results
 */
void ListObjects(const URI &path,
                 const std::string aws_id,
                 const std::string aws_key,
                 std::vector<FileInfo> *out_list) {
  CHECK(path.host.length() != 0) << "bucket name not specified in s3";
  out_list->clear();
  std::vector<std::string> amz;
  std::string date = GetDateString();
  std::string signature = Sign(aws_key, "GET", "", "", date, amz,
                               std::string("/") + path.host + "/");    
  
  std::ostringstream sauth, sdate, surl;
  std::ostringstream result;
  sauth << "Authorization: AWS " << aws_id << ":" << signature;
  sdate << "Date: " << date;
  surl << "http://" << path.host << ".s3.amazonaws.com"
       << "/?delimiter=/&prefix=" << RemoveBeginSlash(path.name);
  // make request
  CURL *curl = curl_easy_init();
  curl_slist *slist = NULL;
  slist = curl_slist_append(slist, sdate.str().c_str());
  slist = curl_slist_append(slist, sauth.str().c_str());
  CHECK(curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist) == CURLE_OK);
  CHECK(curl_easy_setopt(curl, CURLOPT_URL, surl.str().c_str()) == CURLE_OK);
  CHECK(curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L) == CURLE_OK);
  CHECK(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteSStreamCallback) == CURLE_OK);
  CHECK(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result) == CURLE_OK);
  CHECK(curl_easy_perform(curl) == CURLE_OK);
  curl_slist_free_all(slist);
  curl_easy_cleanup(curl);
  // parse xml
  std::string ret = result.str();
  if (ret.find("<Error>") != std::string::npos) {
    LOG(FATAL) << ret;
  }
  {// get files
    XMLIter xml(ret.c_str());
    XMLIter data;
    CHECK(xml.GetNext("IsTruncated", &data)) << "missing IsTruncated";
    CHECK(data.str() == "false") << "the returning list is truncated";
    while (xml.GetNext("Contents", &data)) {
      FileInfo info;
      info.path = path;
      XMLIter value;
      CHECK(data.GetNext("Key", &value));
      // add root path to be consistent with other filesys convention
      info.path.name = '/' + value.str();
      CHECK(data.GetNext("Size", &value));
      info.size = static_cast<size_t>(atol(value.str().c_str()));
      info.type = kFile;
      out_list->push_back(info);
    }
  }
  {// get directories
    XMLIter xml(ret.c_str());
    XMLIter data;
    while (xml.GetNext("CommonPrefixes", &data)) {
      FileInfo info;
      info.path = path;
      XMLIter value;
      CHECK(data.GetNext("Prefix", &value));
      // add root path to be consistent with other filesys convention
      info.path.name = '/' + value.str();
      info.size = 0; info.type = kDirectory;
      out_list->push_back(info);
    }
  }
}
}  // namespace s3

S3FileSystem::S3FileSystem() {
  const char *keyid = getenv("AWS_ACCESS_KEY_ID");
  const char *seckey = getenv("AWS_SECRET_ACCESS_KEY");
  if (keyid == NULL) {
    LOG(FATAL) << "Need to set enviroment variable AWS_ACCESS_KEY_ID to use S3";
  }
  if (seckey == NULL) {
    LOG(FATAL) << "Need to set enviroment variable AWS_SECRET_ACCESS_KEY to use S3";
  }
  aws_access_id_ = keyid;
  aws_secret_key_ = seckey;
}

FileInfo S3FileSystem::GetPathInfo(const URI &path) {
  std::vector<FileInfo> files;
  s3::ListObjects(path,  aws_access_id_, aws_secret_key_, &files);
  std::string pdir = path.name + '/';
  for (size_t i = 0; i < files.size(); ++i) {
    if (files[i].path.name == path.name) return files[i];
    if (files[i].path.name == pdir) return files[i];
  }
  LOG(FATAL) << "S3FileSytem.GetPathInfo cannot find information about " + path.str();
  return files[0];
}
void S3FileSystem::ListDirectory(const URI &path, std::vector<FileInfo> *out_list) {
  if (path.name[path.name.length() - 1] == '/') {
    s3::ListObjects(path, aws_access_id_,
                    aws_secret_key_, out_list);
    return;
  }
  std::vector<FileInfo> files;
  std::string pdir = path.name + '/';
  out_list->clear();
  s3::ListObjects(path, aws_access_id_,
                  aws_secret_key_, &files);
  for (size_t i = 0; i < files.size(); ++i) {
    if (files[i].path.name == path.name) {
      CHECK(files[i].type == kFile);
      out_list->push_back(files[i]);
      return;
    }
    if (files[i].path.name == pdir) {
      CHECK(files[i].type == kDirectory);       
      s3::ListObjects(files[i].path, aws_access_id_,
                      aws_secret_key_, out_list);
      return;
    }
  }  
}

IStream *S3FileSystem::Open(const URI &path, const char* const flag) {
  using namespace std;
  if (!strcmp(flag, "r") || !strcmp(flag, "rb")) {
    return new s3::ReadStream(path, aws_access_id_, aws_secret_key_);
  } else if (!strcmp(flag, "w") || !strcmp(flag, "wb")) {
    return new s3::WriteStream(path, aws_access_id_, aws_secret_key_);
  }else {
    CHECK(false) << "S3FileSytem.Open do not support flag " << flag;    
    return NULL;
  }
}

IStream *S3FileSystem::OpenPartForRead(const URI &path, size_t begin_bytes) {
  s3::ReadStream *fs = new s3::ReadStream(path, aws_access_id_, aws_secret_key_);
  fs->Seek(begin_bytes);
  return fs;
}
}  // namespace io
}  // namespace dmlc