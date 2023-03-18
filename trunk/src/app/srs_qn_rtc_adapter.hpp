#ifndef QN_APP_RTC_HPP
#define QN_APP_RTC_HPP

#include <vector>
#include <unordered_map>
#include <string>
#include <thread>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <inttypes.h>

#include <srs_core.hpp>
#include <srs_protocol_st.hpp>
#include <srs_protocol_rtmp_stack.hpp>
#include <srs_kernel_rtc_rtp.hpp>
#include <srs_app_hourglass.hpp>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

class SrsRtcSource;
class SrsRtcSourceDescription;

std::string qn_get_play_stream(const std::string& stream);
std::string qn_get_origin_stream(const std::string& stream);
bool qn_is_play_stream(const std::string& stream);
bool qn_is_play_stream2(const SrsRequest* req);

class QnDataPacket
{
public:
    // 内部分配空间
    QnDataPacket(uint32_t size);
    // 外部传入，在析构时会释放指针指向的空间
    QnDataPacket(char* data, uint32_t size);
    QnDataPacket(char* data, uint32_t size, void(*pfree)(char*, uint32_t));
    ~QnDataPacket();

    char* Data() { return data_; };
    uint32_t Size() { return size_; };
private:
    void(*pfree_)(char*, uint32_t);
    char* data_;
    uint32_t size_;
};

typedef std::shared_ptr<QnDataPacket> QnDataPacket_SharePtr;

class QnRtcData
{
public:
    void SetStreamUrl(const std::string& s) { stream_url_ = s; };
    std::string& StreamUrl() { return stream_url_; };
    void SetType(int32_t type) { type_ = type; };
    int32_t Type() { return type_; };
    json& Head() { return head_; };
    void SetPayload(const QnDataPacket_SharePtr& packet) { packet_ = packet; };
    QnDataPacket_SharePtr Payload() { return packet_; };
private:
    std::string stream_url_;
    int32_t type_;
    json head_;
    QnDataPacket_SharePtr packet_;
};

typedef std::shared_ptr<QnRtcData> QnRtcData_SharePtr;


// mb20230308 自定义rtc consumer承接rtc数据
class QnRtcConsumer : public ISrsFastTimer
{
public:
    QnRtcConsumer(SrsRtcSource* s);
    ~QnRtcConsumer();

    // When source id changed, notice client to print.
    void update_source_id();

    srs_error_t on_publish();
    srs_error_t on_unpublish();
    
    // Put RTP packet into queue.
    // @note We do not drop packet here, but drop it in sender.
    srs_error_t enqueue(SrsRtpPacket* pkt);

    void on_stream_change(SrsRtcSourceDescription* desc);

    std::string& source_stream_url();

    void Dump();

private:
    srs_error_t on_timer(srs_utime_t interval);

private:
    SrsRtcSource* source_;
    std::string stream_url_;
    std::string source_id_;
    uint64_t identity_;
    uint64_t unique_id_;
    int64_t aud_packets_;
    int64_t vid_packets_;
    int64_t aud_bytes_;
    int64_t vid_bytes_;
    int64_t aud_packet_tick_;
    int64_t vid_packet_tick_;
    float   aud_packets_ps_;
    float   aud_bitrate_;
    float   vid_packets_ps_;
    float   vid_bitrate_;
};

// mb20230308 自定义rtc producer，将rtp包提供给SrsRtcSource
class QnRtcProducer : public ISrsRtspPacketDecodeHandler, public ISrsFastTimer
{
public:
    QnRtcProducer(SrsRtcSource* s);
    ~QnRtcProducer();

    srs_error_t on_publish();
    void on_unpublish();
    
    srs_error_t on_data(const QnRtcData_SharePtr& rtc_data);

    std::string& source_stream_url();
    void Dump();

    virtual void on_before_decode_payload(SrsRtpPacket* pkt, SrsBuffer* buf, ISrsRtpPayloader** ppayload, SrsRtspPacketPayloadType* ppt);
private:
    srs_error_t on_timer(srs_utime_t interval);

private:
    SrsRtcSource* source_;
    std::string stream_url_;
    std::string source_id_;
    uint64_t identity_;
    uint64_t unique_id_;

    uint32_t audio_ssrc_;
    uint32_t video_ssrc_;
    uint8_t audio_payload_type_;
    uint8_t video_payload_type_;

    int64_t aud_packets_;
    int64_t vid_packets_;
    int64_t aud_bytes_;
    int64_t vid_bytes_;
    int64_t aud_packet_tick_;
    int64_t vid_packet_tick_;
    float   aud_packets_ps_;
    float   aud_bitrate_;
    float   vid_packets_ps_;
    float   vid_bitrate_;
};


class QnReqStream
{
public:
    bool enable;
    bool published;
    std::vector<void*> users;
    QnRtcProducer* producer;
};

class QnPubStream
{
public:
    bool published;
    QnRtcConsumer* consumer;
};

class TransMsg
{
public:
    std::string stream_url;
    int32_t type;
    QnDataPacket_SharePtr packet;
};

/*************************************************************
 | total size(4bytes) | json size(4bytes) | json | raw data | 
**************************************************************/
class QnTransport;
class QnRtcManager : public ISrsCoroutineHandler, public ISrsFastTimer
{
public:
    static QnRtcManager* Instance();

    // 请求媒体流，对应producer
    srs_error_t RequestStream(SrsRequest* req, void* user);
    srs_error_t StopRequestStream(SrsRequest* req, void* user);
    
    // consumer用于导出媒体流
    srs_error_t AddConsumer(QnRtcConsumer* consumer);
    void StartPublish(const std::string& stream_url);
    void StopPublish(const std::string& stream_url);

    srs_error_t OnRtcData(const QnRtcData_SharePtr& rtc_data);

    virtual srs_error_t cycle();

private:
    void StartSubscribe(const std::string& stream_url);
    void StopSubscibe(const std::string& stream_url);

private:
    QnRtcManager();
    virtual ~QnRtcManager();

    srs_error_t NewProducer(SrsRequest* req, QnRtcProducer* &producer);
    srs_error_t OnProducerData(const std::string& stream_url, int32_t type, const QnDataPacket_SharePtr& packet);
    srs_error_t on_timer(srs_utime_t interval);

private:
    SrsCoroutine* trd_;
    QnTransport* transport_;
    uint64_t send_unique_id_;
    uint64_t recv_unique_id_;
    srs_cond_t consumer_data_cond_;
    std::vector<QnRtcData_SharePtr> vec_consumer_data_;
    std::unordered_map<std::string, QnPubStream*> map_pub_streams_;
    std::unordered_map<std::string, QnReqStream*> map_req_streams_;
};


typedef std::function<void (const std::string& flag, int32_t type, const QnDataPacket_SharePtr& packet)> TransRecvCbType;

class QnTransport
{
public:
    QnTransport(const std::string& name, const TransRecvCbType& callback);
    virtual ~QnTransport();

    virtual uint32_t GetResverdSize() = 0;
    virtual srs_error_t Send(const std::string& stream_url, int32_t type, const QnDataPacket_SharePtr& packet) = 0;

    uint32_t MakeCheckSum32(uint8_t* pdata, uint32_t dataLen);

protected:
    std::string name_;
    TransRecvCbType recv_callback_;
};

class QnLoopTransport : public QnTransport, public ISrsCoroutineHandler
{
public:
    QnLoopTransport(const std::string& name, const TransRecvCbType& callback);
    ~QnLoopTransport();

    virtual uint32_t GetResverdSize();
    virtual srs_error_t Send(const std::string& stream_url, int32_t type, const QnDataPacket_SharePtr& packet);

private:
    virtual srs_error_t cycle();

private:
    SrsCoroutine* trd_;
    srs_cond_t packet_cond_;
    std::vector<TransMsg*> vec_packets_;
};


///////////////////////////////////////////////////////////////////////////////////

class StreamSender;
class StreamReceiver;

class QnSocketPairTransport : public QnTransport, public ISrsCoroutineHandler
{
public:
    QnSocketPairTransport(const std::string& name, const TransRecvCbType& callback);
    ~QnSocketPairTransport();

    virtual uint32_t GetResverdSize();
    virtual srs_error_t Send(const std::string& stream_url, int32_t type, const QnDataPacket_SharePtr& packet);

private:
    virtual srs_error_t cycle();
    void thread_process();
    void deal_publish_msg(TransMsg* msg);
    void deal_request_msg(TransMsg* msg);

private:
    int fds_[2];
    srs_netfd_t rwfd_;
    SrsCoroutine* trd_;
    srs_cond_t packet_cond_;

    std::mutex wt_mutex_;
    std::thread* trans_thread_;
    
    std::string gate_server_;
    std::unordered_map<std::string, StreamSender*> map_stream_senders_;
    std::unordered_map<std::string, StreamReceiver*> map_stream_receivers_;
};

class StreamSender
{
public:
    StreamSender(const std::string gate_server, const std::string& stream_url) { 
        gate_server_ = gate_server;
        stream_url_ = stream_url; 
    };

    virtual ~StreamSender() {};

    virtual srs_error_t Start() = 0;
    virtual void Stop() = 0;

    virtual srs_error_t Send(TransMsg* msg) = 0;

protected:
    std::string gate_server_;
    std::string stream_url_;
};

class HttpStreamSender : public StreamSender
{
public:
    HttpStreamSender(const std::string gate_server, const std::string& stream_url);
    ~HttpStreamSender();

    srs_error_t Start();
    void Stop();
    srs_error_t Send(TransMsg* msg);

    size_t SendMoreCallback(char *dest, size_t size, size_t nmemb);

private:
    void SendProc();
    void CleanInput();

private:
    bool started_;
    bool wait_quit_;
    uint64_t session_;
    int64_t tick_start_;
    bool first_data_;
    std::thread* thread_;
    uint8_t* last_data_;
    size_t last_data_size_;
    size_t last_data_offset_;
    std::mutex mutex_;
    std::condition_variable cond_var_; 
    std::vector<TransMsg*> vec_msgs_;
};

typedef std::function<void (const std::string& flag, TransMsg* msg)> StreamRecvCbType;
class StreamReceiver
{
public:
    StreamReceiver(const std::string gate_server, const std::string& stream_url, const StreamRecvCbType& callback) {
        gate_server_ = gate_server;
        stream_url_ = stream_url; 
        recv_callback_ = callback;
    };

    virtual ~StreamReceiver() {};

    virtual srs_error_t Start() = 0;
    virtual void Stop() = 0;

protected:
    std::string gate_server_;
    std::string stream_url_;
    StreamRecvCbType recv_callback_;
};

class HttpStreamReceiver : public StreamReceiver
{
public:
    HttpStreamReceiver(const std::string gate_server, const std::string& stream_url, const StreamRecvCbType& callback);
    ~HttpStreamReceiver();

    srs_error_t Start();
    void Stop();

    size_t RecvMoreCallback(char *dest, size_t size, size_t nmemb);

private:
    void StopPublish();
    void RecvProc();

private:
    bool started_;
    bool wait_quit_;
    uint64_t session_;
    int64_t tick_start_;
    bool first_data_;
    uint32_t retry_count_;
    uint32_t multi_timeouts_;
    void* multi_handle_;
    std::thread* thread_;
    std::mutex mutex_;
    char* buf_write_;
    uint32_t data_size_;
    uint32_t buf_offset_;
};

#endif /* QN_APP_RTC_HPP */