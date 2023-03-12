#include <algorithm>
#include <exception>
#include <srs_qn_rtc_adapter.hpp>
#include <srs_app_hybrid.hpp>
#include <srs_app_server.hpp>
#include <srs_app_rtc_source.hpp>

#define json_value(type, v, tag) type v; const std::string tag_##v = #tag
#define json_value_str(type, v, tag) type v; const std::string tag_##v = tag
#define json_tag(v) tag_##v


const std::string PLAY_STREAM_TAG = "^#@!qnplaystream$#@";

// mb20230308 播放stream自动加上不太可能被使用的后缀，而且
// 最好是判断下发布的stream名字，不能包含这个后缀
std::string qn_get_play_stream(const std::string& stream)
{
    return stream + PLAY_STREAM_TAG;
}

std::string qn_get_origin_stream(const std::string& stream)
{
    std::string::size_type pos;
    if ((pos = stream.find(PLAY_STREAM_TAG)) == std::string::npos) {
        return stream;
    }

    return stream.substr(0, pos);
}

bool qn_is_play_stream(const std::string& stream)
{
    if (stream.find(PLAY_STREAM_TAG) == std::string::npos) {
        return false;
    }
    return true;
}

bool qn_is_play_stream2(const SrsRequest* req)
{
    return qn_is_play_stream(req->stream);
}

QnDataPacket::QnDataPacket(uint32_t size)
{
    pfree_ = NULL;
    data_ = new char[size];
    srs_assert(data_);
    size_ = size;
}

QnDataPacket::QnDataPacket(char* data, uint32_t size)
{
    srs_assert(data);
    pfree_ = NULL;
    data_ = data;
    size_ = size;
}

QnDataPacket::QnDataPacket(char* data, uint32_t size, void(*pfree)(char*, uint32_t))
{
    srs_assert(data);
    pfree_ = pfree;
    data_ = data;
    size_ = size;
}

QnDataPacket::~QnDataPacket()
{
    if (!pfree_) {
        delete[] data_;
    } else {
        pfree_(data_, size_);
    }
    
    data_ = NULL;
    size_ = 0;
}


// mb20230308 自定义rtc consumer承接rtc数据
QnRtcConsumer::QnRtcConsumer(SrsRtcSource* s)
{
    source_ = s;
    aud_packets_ = 0;
    vid_packets_ = 0;
    aud_bytes_ = 0;
    vid_bytes_ = 0;
    aud_packet_tick_ = srs_update_system_time();
    vid_packet_tick_ = srs_update_system_time();
    if (_srs_hybrid && _srs_hybrid->timer5s()) {
        _srs_hybrid->timer5s()->subscribe(this);
    }
}

QnRtcConsumer::~QnRtcConsumer()
{
}

void QnRtcConsumer::update_source_id()
{
    srs_trace("QnRtcConsumer of %s, update source_id=%s/%s\n", source_stream_url().c_str(), 
                source_->source_id().c_str(), source_->pre_source_id().c_str());
}

srs_error_t QnRtcConsumer::enqueue(SrsRtpPacket* pkt)
{
    if (pkt->is_keyframe()) {
        srs_trace("QnRtcConsumer of %s, recv key frame\n", source_stream_url().c_str());
    }

    if (pkt->is_audio()) {
        aud_packets_++;
        aud_bytes_ += pkt->payload_bytes();
    } else {
        vid_packets_++;
        vid_bytes_ += pkt->payload_bytes();
    }
    
    srs_freep(pkt);
    return srs_success;
}

void QnRtcConsumer::on_stream_change(SrsRtcSourceDescription* desc)
{
    srs_trace("QnRtcConsumer of %s, on stream change\n", source_stream_url().c_str());
}

std::string QnRtcConsumer::source_stream_url()
{
    SrsCplxError::srs_assert(source_);
    return source_->get_request()->get_stream_url();
}

srs_error_t QnRtcConsumer::on_timer(srs_utime_t interval)
{
    if (aud_packets_ > 0) {
        int64_t now = srs_update_system_time();
        float packets_per_sec = (aud_packets_ * 1000000.0f) / (now - aud_packet_tick_);
        float bytes_per_sec = (aud_bytes_ * 1000000.0f) / (now - aud_packet_tick_);
        aud_packet_tick_ = now;
        aud_packets_ = 0;
        aud_bytes_ = 0;
        srs_trace("QnRtcConsumer of %s, audio packet_ps:%.4f, bitrate:%.2f kbps", source_stream_url().c_str(), 
                    packets_per_sec, (bytes_per_sec * 8) / 1024);
    }

    if (vid_packets_ > 0) {
        int64_t now = srs_update_system_time();
        float packets_per_sec = (vid_packets_ * 1000000.0f) / (now - vid_packet_tick_);
        float bytes_per_sec = (vid_bytes_ * 1000000.0f) / (now - vid_packet_tick_);
        vid_packet_tick_ = now;
        vid_packets_ = 0;
        vid_bytes_ = 0;
        srs_trace("QnRtcConsumer of %s, video packet_ps:%.4f, bitrate:%.2f kbps", source_stream_url().c_str(),  
                    packets_per_sec, (bytes_per_sec * 8) / 1024);
    }

    return srs_success;
}


// mb20230308 自定义rtc producer，将rtp包提供给SrsRtcSource
QnRtcProducer::QnRtcProducer(SrsRtcSource* s)
{
    source_ = s;
}

QnRtcProducer::~QnRtcProducer()
{

}

srs_error_t QnRtcProducer::on_data(const QnRtcData_SharePtr& rtc_data)
{
    return srs_success;
}

std::string QnRtcProducer::source_stream_url()
{
    SrsCplxError::srs_assert(source_);
    return source_->get_request()->get_stream_url();
}


// mb20230308
QnRtcManager::QnRtcManager()
{
    auto recv_callback = [&](const std::string& flag, const QnDataPacket_SharePtr& packet) {
        srs_trace("recv data from %s, size:%u\n", flag.c_str(), packet->Size());
        OnProducerData(packet);
    };

    send_unique_id_ = 1;
    recv_unique_id_ = 0;

    consumer_data_cond_ = srs_cond_new();
    transport_ = new QnLoopTransport("transport", recv_callback);
    trd_ = new SrsSTCoroutine("qnrtc-manager", this);
    trd_->start();
}

QnRtcManager::~QnRtcManager()
{
}

QnRtcManager* QnRtcManager::Instance()
{
    static QnRtcManager* instance = new QnRtcManager;
    return instance;
}

srs_error_t QnRtcManager::RequestStream(SrsRequest* req, void* user)
{
    srs_error_t err = srs_success;

    std::string stream_url = req->get_stream_url();
    srs_trace("request stream %s by user:%p\n", stream_url.c_str(), user);
    auto it = map_req_streams_.find(stream_url);
    if (it != map_req_streams_.end()) {
        QnReqStream* req_stream = it->second;
        std::vector<void*>& users = req_stream->users;
        if (std::find(users.begin(), users.end(), user) != users.end()) {
            srs_warn("user already exist, user:%p, left users:%d\n", user, users.size());
        } else {
            users.push_back(user);
            srs_trace("user inserted, user:%p, left users:%d\n", user, users.size());
            req_stream->enable = true;
        }
        
        return err;
    }

    QnReqStream* req_stream = new QnReqStream;
    req_stream->enable = true;
    req_stream->producer = NULL;
    req_stream->users.push_back(user);
    map_req_streams_[stream_url] = req_stream;

    err = NewProducer(req, req_stream->producer);
    if (err != srs_success) {
        srs_error("request stream error, %s\n", SrsCplxError::description(err).c_str());
        // map_req_streams_.erase(map_req_streams_.find(stream_url));
        // delete req_stream;
        return err;
    }

    srs_trace("user inserted, user:%p, left users:%d\n", user, req_stream->users.size());
    return srs_success;
}

srs_error_t QnRtcManager::StopRequestStream(SrsRequest* req, void* user)
{
    std::string stream_url = req->get_stream_url();
    auto it = map_req_streams_.find(stream_url);
    if (it == map_req_streams_.end()) {
        srs_error("request stream %s not exist, error\n", stream_url.c_str());
    } else {
        srs_trace("stop request stream %s by user:%p\n", stream_url.c_str(), user);
        QnReqStream* req_stream = it->second;
        std::vector<void*>& users = req_stream->users;
        auto it_user = std::find(users.begin(), users.end(), user);
        if (it_user == users.end()) {
            srs_warn("user not exist, user:%p, left users:%d\n", user, users.size());
        } else {
            users.erase(it_user);
            srs_trace("user removed, user:%p, left users:%d\n", user, users.size());
            if (users.empty()) {
                req_stream->enable = false;
            }
        }
    }

    return srs_success;
}

srs_error_t QnRtcManager::AddConsumer(QnRtcConsumer* consumer)
{
    std::string stream_url = consumer->source_stream_url();
    auto it = map_consumers_.find(stream_url);
    if (it == map_consumers_.end()) {
        map_consumers_[stream_url] = consumer;
    }

    return srs_success;
}

// 传入数据必须同步处理完
srs_error_t QnRtcManager::OnConsumerData(const QnRtcData_SharePtr& rtc_data)
{
    vec_consumer_data_.push_back(rtc_data);
    srs_cond_signal(consumer_data_cond_);
    return srs_success;
}

srs_error_t QnRtcManager::cycle()
{
    srs_error_t err = srs_success;
    srs_trace("QnRtcManager thread running \n");

    while (true) {
        if ((err = trd_->pull()) != srs_success) {
            return srs_error_wrap(err, "buffer cache");
        }

        if (vec_consumer_data_.empty()) {
            srs_cond_wait(consumer_data_cond_);
        }

        auto it = vec_consumer_data_.begin();
        if (it == vec_consumer_data_.end()) {
            continue;
        }

        QnRtcData_SharePtr rtc_data = *it;
        vec_consumer_data_.erase(it);

        json& js = rtc_data->Head();
        // 添加系列号以判断是否数据有丢失
        js["rtc_unique_id"] = send_unique_id_++;
        js["rtc_stream_url"] = rtc_data->StreamUrl();

        std::string head = js.dump();
        uint16_t js_size = static_cast<uint16_t>(head.size());
        uint16_t js_offset = 4 + 2 + 2;

        /****************************************************************************************
         | total size(4bytes) | json size(2bytes) | json offset(2bytes) | *** | json | raw data | 
        *****************************************************************************************/
        uint32_t total_size = js_size + 4 + 2 + 2 + rtc_data->DataPacket()->Size();
        QnDataPacket_SharePtr packet = std::make_shared<QnDataPacket>(total_size);

        // big endian
        char* data = packet->Data();

        // write total_size
        *data++ = ((total_size >> 24) & 0xff);
        *data++ = ((total_size >> 16) & 0xff);
        *data++ = ((total_size >> 8) & 0xff);
        *data++ = (total_size & 0xff);

        // json size
        *data++ = ((js_size >> 8) & 0xff);
        *data++ = (js_size & 0xff);
        // json offset
        *data++ = ((js_offset >> 8) & 0xff);
        *data++ = (js_offset & 0xff);
        
        // json data
        memcpy(data, head.c_str(), js_size);
        data += js_size;

        // raw payload data
        memcpy(data, rtc_data->DataPacket()->Data(), rtc_data->DataPacket()->Size());

        if (transport_) {
            transport_->Send(packet);
        }
    }

    srs_trace("QnRtcManager thread quit... \n");
    return err;
}

srs_error_t QnRtcManager::NewProducer(SrsRequest* req, QnRtcProducer* &producer)
{
    producer = NULL;
    srs_error_t err = srs_success;
    SrsRtcSource* source = NULL;

    if ((err = _srs_rtc_sources->fetch_or_create(req, &source)) != srs_success) {
        return srs_error_wrap(err, "create source");
    }

    SrsLiveSource *rtmp = NULL;
    if ((err = _srs_sources->fetch_or_create(req, _srs_hybrid->srs()->instance(), &rtmp)) != srs_success) {
        return srs_error_wrap(err, "create source");
    }

    // Disable GOP cache for RTC2RTMP bridge, to keep the streams in sync,
    // especially for stream merging.
    rtmp->set_cache(false);

    SrsRtmpFromRtcBridge *bridge = new SrsRtmpFromRtcBridge(rtmp);
    if ((err = bridge->initialize(req)) != srs_success) {
        srs_freep(bridge);
        return srs_error_wrap(err, "create bridge");
    }

    source->set_bridge(bridge);

    producer = new QnRtcProducer(source);

    return srs_success;
}

#define json_have(j, x) (j.find(x) != j.end())
#define json_do_default(v, x, y)    do {    \
    try { \
        v = x; \
    } catch (const std::exception& e) { \
        srs_error("%s error, %s\n", #x, e.what()); \
        v = y; \
    } \
} while(0)

srs_error_t QnRtcManager::OnProducerData(const QnDataPacket_SharePtr& packet)
{
    srs_error_t err = srs_success;

    // big endian
    char* data = packet->Data();

    uint32_t total_size = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    uint16_t js_size = (data[4] << 8) | data[5];
    uint16_t js_offset = (data[6] << 8) | data[7];

    QnRtcData_SharePtr rtc_data = std::make_shared<QnRtcData>();

    json& js = rtc_data->Head();
    std::string head(data + js_offset, js_size);
    js = json::parse(head);

    if (!json_have(js, "rtc_unique_id") || !json_have(js, "rtc_stream_url")) {
        srs_error("producer data no rtc_unique_id or rtc_stream_url, error");
        return err;
    }

    uint64_t unique_id;
    json_do_default(unique_id, js["rtc_unique_id"], 0);
    if (unique_id != recv_unique_id_ + 1) {
        srs_warn("unique id jumped, %lld --> %lld\n", recv_unique_id_, unique_id);
    }
    recv_unique_id_ = unique_id;

    std::string stream_url;
    json_do_default(stream_url, js["rtc_stream_url"], "^&unknow");
    stream_url = qn_get_play_stream(stream_url);

    auto it = map_req_streams_.find(stream_url);
    if (it == map_req_streams_.end()) {
        return err;
    }

    QnReqStream* req_stream = it->second;
    if (!req_stream->enable) {
        return err;
    }

    if (!req_stream->producer) {
        srs_warn("producer not exist, stream:%s\n", stream_url.c_str());
        return err;
    }

    char* payload = data + js_offset + js_size;
    uint32_t payload_size = total_size - js_size - js_offset;
    QnDataPacket_SharePtr payload_packet = std::make_shared<QnDataPacket>(payload_size);
    memcpy(payload_packet->Data(), payload, payload_size);

    rtc_data->SetDataPacket(payload_packet);
    rtc_data->SetStreamUrl(stream_url);

    req_stream->producer->on_data(rtc_data);

    return err;
}


QnTransport::QnTransport(const std::string& name, const TransRecvCbType& callback)
{
    name_ = name;
    recv_callback_ = callback;
}

QnTransport::~QnTransport()
{
}


QnLoopTransport::QnLoopTransport(const std::string& name, const TransRecvCbType& callback) :
                    QnTransport(name, callback)
{
    packet_cond_ = srs_cond_new();
    trd_ = new SrsSTCoroutine("loop-transport", this);
    trd_->start();
}

QnLoopTransport::~QnLoopTransport()
{
}

srs_error_t QnLoopTransport::Send(const QnDataPacket_SharePtr& packet)
{
    srs_trace("LoopTransport send %u bytes\n", packet->Size());
    vec_packets_.push_back(packet);
    srs_cond_signal(packet_cond_);
    return srs_success;
}

srs_error_t QnLoopTransport::cycle()
{
    srs_error_t err = srs_success;
    srs_trace("QnLoopTransport thread running \n");

    while (true) {
        if ((err = trd_->pull()) != srs_success) {
            return srs_error_wrap(err, "buffer cache");
        }

        if (vec_packets_.empty()) {
            srs_cond_wait(packet_cond_);
        }

        auto it = vec_packets_.begin();
        if (it == vec_packets_.end()) {
            continue;
        }

        QnDataPacket_SharePtr packet = *it;
        vec_packets_.erase(it);

        if (recv_callback_) {
            recv_callback_(name_, packet);
        }
    }

    srs_trace("QnLoopTransport thread quit... \n");
    return err;
}