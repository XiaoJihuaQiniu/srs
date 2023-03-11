#include <srs_qn_rtc_adapter.hpp>
#include <srs_app_hybrid.hpp>
#include <srs_app_server.hpp>
#include <srs_app_rtc_source.hpp>

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

srs_error_t QnRtcProducer::on_data(char* data, int size)
{
    return srs_success;
}

std::string QnRtcProducer::source_stream_url()
{
    SrsCplxError::srs_assert(source_);
    return source_->get_request()->get_stream_url();
}


// mb20230308
QnTransport::QnTransport()
{
}

QnTransport::~QnTransport()
{
}

QnTransport* QnTransport::Instance()
{
    static QnTransport* instance = new QnTransport;
    return instance;
}

srs_error_t QnTransport::RequestStream(SrsRequest* req)
{
    srs_error_t err = srs_success;

    std::string stream_url = req->get_stream_url();
    srs_trace("request stream %s\n", stream_url.c_str());
    auto it = map_req_streams_.find(stream_url);
    if (it != map_req_streams_.end()) {
        it->second->enable = true;
        return err;
    }

    QnReqStream* req_stream = new QnReqStream;
    req_stream->enable = true;
    req_stream->producer = NULL;
    map_req_streams_[stream_url] = req_stream;

    err = NewProducer(req, req_stream->producer);
    if (err != srs_success) {
        srs_error("request stream error, %s\n", SrsCplxError::description(err).c_str());
        map_req_streams_.erase(map_req_streams_.begin());
        delete req_stream;
        return err;
    }

    return srs_success;
}

srs_error_t QnTransport::StopRequestStream(SrsRequest* req)
{
    std::string stream_url = req->get_stream_url();
    auto it = map_req_streams_.find(stream_url);
    if (it == map_req_streams_.end()) {
        srs_error("request stream %s not exist, error\n", stream_url.c_str());
    } else {
        srs_trace("stop request stream %s\n", stream_url.c_str());
        it->second->enable = false;
    }

    return srs_success;
}

srs_error_t QnTransport::AddConsumer(QnRtcConsumer* consumer)
{
    std::string stream_url = consumer->source_stream_url();
    auto it = map_consumers_.find(stream_url);
    if (it == map_consumers_.end()) {
        map_consumers_[stream_url] = consumer;
    }

    return srs_success;
}

srs_error_t QnTransport::on_consumer_data(QnConsumerData* data)
{
    vec_consumer_data_.push_back(data);
    return srs_success;
}

srs_error_t QnTransport::NewProducer(SrsRequest* req, QnRtcProducer* &producer)
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
