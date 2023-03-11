#ifndef QN_APP_RTC_HPP
#define QN_APP_RTC_HPP

#include <vector>
#include <map>
#include <string>
#include <functional>
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

class QnRtcMsg
{
public:
    void SetStreamUrl(const std::string& s) { stream_url_ = s; };
    std::string& StreamUrl() { return stream_url_; };

    void SetType(uint32_t type) { type_ = type; };
    uint32_t Type() { return type_; };

    json& Head() { return head_; };

    void SetData(const QnDataPacket_SharePtr& data) { data_ = data; };
    QnDataPacket_SharePtr Data() { return data_; };
private:
    std::string stream_url_;
    uint32_t type_;
    json head_;
    QnDataPacket_SharePtr data_;
};

typedef std::shared_ptr<QnRtcMsg> QnRtcMsg_SharePtr;


// mb20230308 自定义rtc consumer承接rtc数据
class QnRtcConsumer : public ISrsFastTimer
{
public:
    QnRtcConsumer(SrsRtcSource* s);
    ~QnRtcConsumer();

    // When source id changed, notice client to print.
    void update_source_id();
    
    // Put RTP packet into queue.
    // @note We do not drop packet here, but drop it in sender.
    srs_error_t enqueue(SrsRtpPacket* pkt);

    void on_stream_change(SrsRtcSourceDescription* desc);

    std::string source_stream_url();

private:
    srs_error_t on_timer(srs_utime_t interval);

private:
    SrsRtcSource* source_;
    std::vector<SrsRtpPacket*> queue_;
    int64_t aud_packets_;
    int64_t vid_packets_;
    int64_t aud_bytes_;
    int64_t vid_bytes_;
    int64_t aud_packet_tick_;
    int64_t vid_packet_tick_;
};

// mb20230308 自定义rtc producer，将rtp包提供给SrsRtcSource
class QnRtcProducer
{
public:
    QnRtcProducer(SrsRtcSource* s);
    ~QnRtcProducer();

    srs_error_t on_data(const QnRtcMsg_SharePtr& rtc_msg);

    std::string source_stream_url();

private:
    SrsRtcSource* source_;
};


// mb20230308
class QnReqStream
{
public:
    bool enable;
    std::vector<void*> users;
    QnRtcProducer* producer;
};

/********************************************************************************************
  | total size(4bytes) | json offset(2bytes) | json size(2bytes) | json | raw payload data | 
********************************************************************************************/
class QnTransport;
class QnRtcManager
{
public:
    static QnRtcManager* Instance();

    srs_error_t RequestStream(SrsRequest* req, void* user);
    srs_error_t StopRequestStream(SrsRequest* req, void* user);
    
    srs_error_t AddConsumer(QnRtcConsumer* consumer);
    srs_error_t OnConsumerData(const QnRtcMsg_SharePtr& rtc_msg);

private:
    QnRtcManager();
    virtual ~QnRtcManager();

    srs_error_t NewProducer(SrsRequest* req, QnRtcProducer* &producer);

private:
    QnTransport* transport_;
    std::vector<QnRtcMsg_SharePtr> vec_consumer_data_;
    std::map<std::string, QnRtcConsumer*> map_consumers_;
    std::map<std::string, QnReqStream*> map_req_streams_;
};


typedef std::function<void (const std::string& flag, char* data, uint32_t size)> TransRecvCbType;

class QnTransport
{
public:
    QnTransport(const std::string& name, const TransRecvCbType& callback);
    virtual ~QnTransport();

    virtual srs_error_t Send(char* data, uint32_t size) = 0;

private:
    std::string name_;
    TransRecvCbType recv_callback_;
};

class QnSimpleTransport : public QnTransport
{
public:
    QnSimpleTransport(const std::string& name, const TransRecvCbType& callback);
    ~QnSimpleTransport();

    virtual srs_error_t Send(char* data, uint32_t size);
};

#endif /* QN_APP_RTC_HPP */