#ifndef QN_APP_RTC_HPP
#define QN_APP_RTC_HPP

#include <srs_core.hpp>

#include <vector>
#include <map>
#include <string>
#include <inttypes.h>

#include <srs_protocol_st.hpp>
#include <srs_protocol_rtmp_stack.hpp>
#include <srs_kernel_rtc_rtp.hpp>
#include <srs_app_hourglass.hpp>

class SrsRtcSource;
class SrsRtcSourceDescription;

std::string qn_get_play_stream(const std::string& stream);
bool qn_is_play_stream(const std::string& stream);

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

    srs_error_t on_data(char* data, int size);

    std::string source_stream_url();

private:
    SrsRtcSource* source_;
};

// mb20230308 与服务器之间收发数据
class QnConsumerData
{
public:
    /* head is json format */
    QnConsumerData(const QnRtcConsumer* consumer, int32_t type, char* data, char* head, 
                            uint32_t dsize, uint32_t hsize) : 
                            consumer_(consumer), type_(type), head_(head), data_(data), 
                            date_size_(dsize), head_size_(hsize) {
    };
    
    ~QnConsumerData() {
        consumer_ = NULL;
        if (head_) {
            delete[] head_;
            head_ = NULL;
        }
        if (data_) {
            delete[] data_;
            data_ = NULL;
        }
    };

private:
    const QnRtcConsumer* consumer_;
    int32_t type_;      // audio or video
    char* head_;        // json 格式数据
    char* data_;
    uint32_t date_size_;
    uint32_t head_size_;
};

// mb20230308
class QnReqStream
{
public:
    bool enable;
    std::vector<void*> users;
    QnRtcProducer* producer;
};

/***************************************************************************
  | total size(4bytes) | head size(4bytes) | head (json) | payload data | 
*****************************************************************************/
class QnTransport
{
public:
    static QnTransport* Instance();

    srs_error_t RequestStream(SrsRequest* req, void* user);
    srs_error_t StopRequestStream(SrsRequest* req, void* user);
    
    srs_error_t AddConsumer(QnRtcConsumer* consumer);
    
    srs_error_t on_consumer_data(QnConsumerData* data);

private:
    QnTransport();
    virtual ~QnTransport();

    srs_error_t NewProducer(SrsRequest* req, QnRtcProducer* &producer);

private:
    std::vector<QnConsumerData*> vec_consumer_data_;
    std::map<std::string, QnRtcConsumer*> map_consumers_;
    std::map<std::string, QnReqStream*> map_req_streams_;
};

#endif /* QN_APP_RTC_HPP */