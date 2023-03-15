#include <algorithm>
#include <exception>
#include <unistd.h>
#include <srs_qn_rtc_adapter.hpp>
#include <srs_app_hybrid.hpp>
#include <srs_app_server.hpp>
#include <srs_app_rtc_source.hpp>

#define json_value(type, v, tag) type v; const std::string tag_##v = #tag
#define json_value_str(type, v, tag) type v; const std::string tag_##v = tag
#define json_tag(v) tag_##v

#define json_have(j, x) (j.find(x) != j.end())
#define json_do_default(v, x, y)    do {    \
    try { \
        v = x; \
    } catch (const std::exception& e) { \
        srs_error("%s error, %s\n", #x, e.what()); \
        v = y; \
    } \
} while(0)


//const std::string PLAY_STREAM_TAG = "^#@qnplaystream$#@";
const std::string PLAY_STREAM_TAG = "--qnplaystream";

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

enum EmRtcDataType
{
    en_RtcDataType_Media = 0,           // 音视频数据
    en_RtcDataType_NewStream,           // 新的数据流
    en_RtcDataType_DeleteStream,        // 停止数据流
    
    en_RtcDataType_butt
};

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

const std::string ID = "id";
const std::string PACKET_ID = "packet_id";
const std::string ASTIME = "astime";
const std::string MTYPE = "mtype";
const std::string PAYLOAD_TYPE = "pt";
const std::string MARK_BIT = "mark";
const std::string KEY_FRAME = "key";

// mb20230308 自定义rtc consumer承接rtc数据
QnRtcConsumer::QnRtcConsumer(SrsRtcSource* s)
{
    SrsCplxError::srs_assert(s);
    source_ = s;
    stream_url_ = s->get_request()->get_stream_url();
    source_id_ = "unknow";
    identity_ = srs_update_system_time();
    unique_id_ = 1;
    aud_packets_ = 0;
    vid_packets_ = 0;
    aud_bytes_ = 0;
    vid_bytes_ = 0;
    aud_packet_tick_ = srs_update_system_time();
    vid_packet_tick_ = srs_update_system_time();
    aud_packets_ps_ = 0.0f;
    aud_bitrate_ = 0.0f;
    vid_packets_ps_ = 0.0f;
    vid_bitrate_ = 0.0f;

    if (_srs_hybrid && _srs_hybrid->timer1s()) {
        _srs_hybrid->timer1s()->subscribe(this);
    }
}

QnRtcConsumer::~QnRtcConsumer()
{
}

void QnRtcConsumer::update_source_id()
{
    srs_trace("QnRtcConsumer of %s, update source_id=%s/%s", stream_url_.c_str(), 
                source_->source_id().c_str(), source_->pre_source_id().c_str());
    source_id_ = source_->source_id().get_value();
}

srs_error_t QnRtcConsumer::enqueue(SrsRtpPacket* pkt)
{
    srs_error_t err = srs_success;

    if (pkt->is_keyframe() && pkt->header.get_marker()) {
        srs_trace("--> QnRtcConsumer of %s, recv key frame, ts:%lld", stream_url_.c_str(), pkt->get_avsync_time());
    }

    if (pkt->is_audio()) {
        aud_packets_++;
        aud_bytes_ += pkt->payload_bytes();
    } else {
        vid_packets_++;
        vid_bytes_ += pkt->payload_bytes();
    }

    char* buffer = new char[kRtpPacketSize];
    srs_assert(buffer);

    SrsBuffer enc_buffer(buffer, kRtpPacketSize);
    if ((err = pkt->encode(&enc_buffer)) != srs_success) {
        delete[] buffer;
        srs_error("encode packet error\n");
        return srs_error_wrap(err, "encode packet");
    }
    
    QnDataPacket_SharePtr payload = std::make_shared<QnDataPacket>(buffer, enc_buffer.pos());
    QnRtcData_SharePtr rtc_data = std::make_shared<QnRtcData>();
    rtc_data->SetPayload(payload);
    rtc_data->SetStreamUrl(stream_url_);
    rtc_data->SetType(en_RtcDataType_Media);

    // unique_id
    // video or audio
    // is keyframe (video)
    // is mark bit set (video)
    json& js = rtc_data->Head();
    // 添加系列号以判断是否数据有丢失
    js[ID] = source_id_;
    js[PACKET_ID] = unique_id_++;
    js[ASTIME] = pkt->get_avsync_time();
    js[MTYPE] = pkt->is_audio() ? "audio" : "video";
    
    // srs_trace("++++ stream:%s, pt:%d, audio:%d, key:%d, mark:%d, pack time:%lld\n", stream_url_.c_str(), pkt->payload_type(), 
    //             pkt->is_audio(), pkt->is_keyframe(), pkt->header.get_marker(), pkt->get_avsync_time());

    if (!pkt->is_audio()) {
        js[PAYLOAD_TYPE] = pkt->payload_type();
        js[KEY_FRAME] = pkt->is_keyframe() ? 1 : 0;
        js[MARK_BIT] = pkt->header.get_marker() ? 1 : 0;
    }

    QnRtcManager::Instance()->OnConsumerData(rtc_data);

    return srs_success;
}

void QnRtcConsumer::on_stream_change(SrsRtcSourceDescription* desc)
{
    srs_trace("QnRtcConsumer of %s, on stream change\n", stream_url_.c_str());
    identity_ = srs_update_system_time();
}

std::string& QnRtcConsumer::source_stream_url()
{
    return stream_url_;
}

void QnRtcConsumer::Dump()
{
    srs_trace2("QNDUMP", "consumer stream:%s", stream_url_.c_str());
    srs_trace2("QNDUMP", "audio packet_ps:%.4f, bitrate:%.2f kbps", aud_packets_ps_, aud_bitrate_);
    srs_trace2("QNDUMP", "video packet_ps:%.4f, bitrate:%.2f kbps", vid_packets_ps_, vid_bitrate_);
}

srs_error_t QnRtcConsumer::on_timer(srs_utime_t interval)
{
    if (aud_packets_ > 0) {
        int64_t now = srs_update_system_time();
        float packets_per_sec = ((float)aud_packets_ * SRS_UTIME_SECONDS) / (now - aud_packet_tick_);
        float bytes_per_sec = ((float)aud_bytes_ * SRS_UTIME_SECONDS) / (now - aud_packet_tick_);
        aud_packet_tick_ = now;
        aud_packets_ = 0;
        aud_bytes_ = 0;
        aud_packets_ps_ = packets_per_sec;
        aud_bitrate_ = (bytes_per_sec * 8) / 1024;
        // srs_trace("QnRtcConsumer of %s, audio packet_ps:%.4f, bitrate:%.2f kbps", source_stream_url().c_str(), 
        //             aud_packets_ps_, aud_bitrate_);
    } else {
        aud_packets_ps_ = 0.0f;
        aud_bitrate_ = 0.0f;
    }

    if (vid_packets_ > 0) {
        int64_t now = srs_update_system_time();
        float packets_per_sec = ((float)vid_packets_ * SRS_UTIME_SECONDS) / (now - vid_packet_tick_);
        float bytes_per_sec = ((float)vid_bytes_ * SRS_UTIME_SECONDS) / (now - vid_packet_tick_);
        vid_packet_tick_ = now;
        vid_packets_ = 0;
        vid_bytes_ = 0;
        vid_packets_ps_ = packets_per_sec;
        vid_bitrate_ = (bytes_per_sec * 8) / 1024;
        // srs_trace("QnRtcConsumer of %s, video packet_ps:%.4f, bitrate:%.2f kbps", source_stream_url().c_str(),  
        //             vid_packets_ps_, vid_bitrate_);
    } else {
        vid_packets_ps_ = 0.0f;
        vid_bitrate_ = 0.0f;
    }

    return srs_success;
}


// 以下常量拷贝自 srs_app_rtc_source.cpp
// Firefox defaults as 109, Chrome is 111.
const int kAudioPayloadType     = 111;
const int kAudioChannel         = 2;
const int kAudioSamplerate      = 48000;

// Firefox defaults as 126, Chrome is 102.
const int kVideoPayloadType = 102;
const int kVideoSamplerate  = 90000;

// mb20230308 自定义rtc producer，将rtp包提供给SrsRtcSource
QnRtcProducer::QnRtcProducer(SrsRtcSource* s)
{
    SrsCplxError::srs_assert(s);
    source_ = s;
    stream_url_ = s->get_request()->get_stream_url();
    source_id_ = "unknow";
    unique_id_ = 0;

    // audio track ssrc
    if (true) {
        std::vector<SrsRtcTrackDescription*> descs = source_->get_track_desc("audio", "opus");
        if (!descs.empty()) {
            audio_ssrc_ = descs.at(0)->ssrc_;
        }
        // Note we must use the PT of source, see https://github.com/ossrs/srs/pull/3079
        audio_payload_type_ = descs.empty() ? kAudioPayloadType : descs.front()->media_->pt_;
    }

    // video track ssrc
    if (true) {
        std::vector<SrsRtcTrackDescription*> descs = source_->get_track_desc("video", "H264");
        if (!descs.empty()) {
            video_ssrc_ = descs.at(0)->ssrc_;
        }
        // Note we must use the PT of source, see https://github.com/ossrs/srs/pull/3079
        video_payload_type_ = descs.empty() ? kVideoPayloadType : descs.front()->media_->pt_;
    }

    srs_trace("producer %s,  aud ssrc:%u, pt:%hhu, vid ssrc:%u, pt:%hhu\n", stream_url_.c_str(), 
               audio_ssrc_, audio_payload_type_, video_ssrc_, video_payload_type_);

    aud_packets_ = 0;
    vid_packets_ = 0;
    aud_bytes_ = 0;
    vid_bytes_ = 0;
    aud_packet_tick_ = srs_update_system_time();
    vid_packet_tick_ = srs_update_system_time();
    aud_packets_ps_ = 0.0f;
    aud_bitrate_ = 0.0f;
    vid_packets_ps_ = 0.0f;
    vid_bitrate_ = 0.0f;

    if (_srs_hybrid && _srs_hybrid->timer1s()) {
        _srs_hybrid->timer1s()->subscribe(this);
    }
}

QnRtcProducer::~QnRtcProducer()
{

}

srs_error_t QnRtcProducer::on_publish()
{
    srs_trace("producer %s on publish", stream_url_.c_str());
    return source_->on_publish_qn();
}

void QnRtcProducer::on_unpublish()
{
    srs_trace("producer %s on unpublish", stream_url_.c_str());
    return source_->on_unpublish_qn();
}

srs_error_t QnRtcProducer::on_data(const QnRtcData_SharePtr& rtc_data)
{
    srs_error_t err = srs_success;

    json& js = rtc_data->Head();
    if (!json_have(js, ID) || !json_have(js, PACKET_ID) || 
        !json_have(js, MTYPE) || !json_have(js, ASTIME)) {
        srs_error("producer data no packet_id or pt, error");
        return err;
    }

    std::string source_id;
    json_do_default(source_id, js[ID], "**unknow");
    if (source_id != source_id_) {
        srs_trace("producer %s source id changed, %s -- > %s\n", stream_url_.c_str(), 
                source_id_.c_str(), source_id.c_str());
        source_id_ = source_id;
    }

    uint64_t unique_id;
    json_do_default(unique_id, js[PACKET_ID], 0);
    if (unique_id != unique_id_ + 1) {
        srs_warn("producer %s unique id jumped, %lld --> %lld\n", stream_url_.c_str(), unique_id_, unique_id);
    }
    unique_id_ = unique_id;

    QnDataPacket_SharePtr payload = rtc_data->Payload();
    SrsRtpPacket* pkt = new SrsRtpPacket();
    char* p = pkt->wrap(payload->Data(), payload->Size());
    SrsBuffer buf(p, payload->Size());

    pkt->set_decode_handler(this);
    // TODO
    // pkt->set_extension_types(&extension_types_);
    pkt->header.ignore_padding(false);

    // int pt, key, mark;
    std::string type;
    json_do_default(type, js[MTYPE], "unknow");
    if (type == "video") {
        pkt->frame_type = SrsFrameTypeVideo;
        // json_do_default(pt, js[PAYLOAD_TYPE], 0);
        // json_do_default(key, js[KEY_FRAME], 0);
        // json_do_default(mark, js[MARK_BIT], 0);
        // srs_trace("%s, video packet, unique_id:%llu, pt:%d, key:%d, mark:%d, size:%u\n", stream_url_.c_str(), 
        //             unique_id_, pt, key, mark, payload->Size());
    } else if (type == "audio") {
        pkt->frame_type = SrsFrameTypeAudio;
        // srs_trace("%s, audio packet, unique_id:%llu,  size:%u\n", stream_url_.c_str(), 
        //             unique_id_, payload->Size());
    }

    if ((err = pkt->decode(&buf)) != srs_success) {
        return srs_error_wrap(err, "decode rtp packet");
    }

    int64_t astime;
    json_do_default(astime, js[ASTIME], 0);
    pkt->set_avsync_time(astime);

    if (pkt->is_audio()) {
        pkt->header.set_payload_type(audio_payload_type_);
        pkt->header.set_ssrc(audio_ssrc_);
        aud_packets_++;
        aud_bytes_ += pkt->payload_bytes();
    } else {
        pkt->header.set_payload_type(video_payload_type_);
        pkt->header.set_ssrc(video_ssrc_);
        vid_packets_++;
        vid_bytes_ += pkt->payload_bytes();
    }

    if (pkt->is_keyframe() && pkt->header.get_marker()) {
        srs_trace("<-- QnRtcProducer of %s, recv key frame, ts:%lld", qn_get_origin_stream(stream_url_).c_str(), 
                    pkt->get_avsync_time());
    }
    
    // source_->on_rtp(pkt);
    source_->on_rtp_qn(source_id_, pkt);

    srs_freep(pkt);
    return srs_success;
}

std::string& QnRtcProducer::source_stream_url()
{
    return stream_url_;
}

void QnRtcProducer::Dump()
{
    srs_trace2("QNDUMP", "producer stream:%s", qn_get_origin_stream(stream_url_).c_str());
    srs_trace2("QNDUMP", "audio packet_ps:%.4f, bitrate:%.2f kbps", aud_packets_ps_, aud_bitrate_);
    srs_trace2("QNDUMP", "video packet_ps:%.4f, bitrate:%.2f kbps", vid_packets_ps_, vid_bitrate_);
}

void QnRtcProducer::on_before_decode_payload(SrsRtpPacket* pkt, SrsBuffer* buf, ISrsRtpPayloader** ppayload, SrsRtspPacketPayloadType* ppt)
{
    // No payload, ignore.
    if (buf->empty()) {
        return;
    }

    if (pkt->is_audio()) {
        *ppayload = new SrsRtpRawPayload();
        *ppt = SrsRtspPacketPayloadTypeRaw;
    } else {
        uint8_t v = (uint8_t)(buf->head()[0] & kNalTypeMask);
        pkt->nalu_type = SrsAvcNaluType(v);

        if (v == kStapA) {
            *ppayload = new SrsRtpSTAPPayload();
            *ppt = SrsRtspPacketPayloadTypeSTAP;
        } else if (v == kFuA) {
            *ppayload = new SrsRtpFUAPayload2();
            *ppt = SrsRtspPacketPayloadTypeFUA2;
        } else {
            *ppayload = new SrsRtpRawPayload();
            *ppt = SrsRtspPacketPayloadTypeRaw;
        }
    }
}

srs_error_t QnRtcProducer::on_timer(srs_utime_t interval)
{
    if (aud_packets_ > 0) {
        int64_t now = srs_update_system_time();
        float packets_per_sec = ((float)aud_packets_ * SRS_UTIME_SECONDS) / (now - aud_packet_tick_);
        float bytes_per_sec = ((float)aud_bytes_ * SRS_UTIME_SECONDS) / (now - aud_packet_tick_);
        aud_packet_tick_ = now;
        aud_packets_ = 0;
        aud_bytes_ = 0;
        aud_packets_ps_ = packets_per_sec;
        aud_bitrate_ = (bytes_per_sec * 8) / 1024;
        // srs_trace("QnRtcConsumer of %s, audio packet_ps:%.4f, bitrate:%.2f kbps", source_stream_url().c_str(), 
        //             aud_packets_ps_, aud_bitrate_);
    } else {
        aud_packets_ps_ = 0.0f;
        aud_bitrate_ = 0.0f;
    }

    if (vid_packets_ > 0) {
        int64_t now = srs_update_system_time();
        float packets_per_sec = ((float)vid_packets_ * SRS_UTIME_SECONDS) / (now - vid_packet_tick_);
        float bytes_per_sec = ((float)vid_bytes_ * SRS_UTIME_SECONDS) / (now - vid_packet_tick_);
        vid_packet_tick_ = now;
        vid_packets_ = 0;
        vid_bytes_ = 0;
        vid_packets_ps_ = packets_per_sec;
        vid_bitrate_ = (bytes_per_sec * 8) / 1024;
        // srs_trace("QnRtcConsumer of %s, video packet_ps:%.4f, bitrate:%.2f kbps", source_stream_url().c_str(),  
        //             vid_packets_ps_, vid_bitrate_);
    } else {
        vid_packets_ps_ = 0.0f;
        vid_bitrate_ = 0.0f;
    }

    return srs_success;
}

// mb20230308
QnRtcManager::QnRtcManager()
{
    auto recv_callback = [&](const std::string& flag, const QnDataPacket_SharePtr& packet) {
        // srs_trace("recv data from %s, size:%u\n", flag.c_str(), packet->Size());
        OnProducerData(packet);
    };

    send_unique_id_ = 1;
    recv_unique_id_ = 0;

    consumer_data_cond_ = srs_cond_new();
    transport_ = new QnSocketPairTransport("transport", recv_callback);
    trd_ = new SrsSTCoroutine("qnrtc-manager", this);
    trd_->start();

    if (_srs_hybrid && _srs_hybrid->timer5s()) {
        _srs_hybrid->timer5s()->subscribe(this);
    }
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
            if (!req_stream->enable) {
                srs_assert(req_stream->producer);
                if (req_stream->producer) {
                    req_stream->producer->on_publish();
                }
                req_stream->enable = true;
            }
        }
        
        return err;
    }

    QnReqStream* req_stream = new QnReqStream;
    req_stream->producer = NULL;
    req_stream->users.push_back(user);
    map_req_streams_[stream_url] = req_stream;

    err = NewProducer(req, req_stream->producer);
    if (err != srs_success) {
        srs_error("request stream error, %s\n", SrsCplxError::description(err).c_str());
        // map_req_streams_.erase(map_req_streams_.find(stream_url));
        // delete req_stream;
        // return err;
    }

    req_stream->producer->on_publish();
    req_stream->enable = true;

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
        // todo
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
                if (req_stream->producer){
                    req_stream->producer->on_unpublish();
                }
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

        // srs_trace("consumer data wait send, %d\n", vec_consumer_data_.size());
        auto it = vec_consumer_data_.begin();
        if (it == vec_consumer_data_.end()) {
            continue;
        }

        QnRtcData_SharePtr rtc_data = *it;
        vec_consumer_data_.erase(it);

        json& js = rtc_data->Head();
        // 添加系列号以判断是否数据有丢失
        js["unique_id"] = send_unique_id_++;
        js["stream_url"] = rtc_data->StreamUrl();

        std::string head = js.dump();
        // srs_trace("send js:%s\n", head.c_str());
        uint16_t js_size = static_cast<uint16_t>(head.size());
        // 前面8字节固定，不能改动
        /*************************************************************
         | total size(4bytes) | json size(4bytes) | json | raw data | 
        **************************************************************/
        uint32_t total_size = 8 + js_size + rtc_data->Payload()->Size();
        QnDataPacket_SharePtr packet = std::make_shared<QnDataPacket>(total_size);

        // big endian
        uint8_t* data = (uint8_t*)packet->Data();

        // write total_size
        data[0] = ((total_size >> 24) & 0xff);
        data[1] = ((total_size >> 16) & 0xff);
        data[2] = ((total_size >> 8) & 0xff);
        data[3] = (total_size & 0xff);

        // json size
        data[4] = ((js_size >> 24) & 0xff);
        data[5] = ((js_size >> 16) & 0xff);
        data[6] = ((js_size >> 8) & 0xff);
        data[7] = (js_size & 0xff);
        
        // json data
        memcpy(data + 8, head.c_str(), js_size);

        // raw payload data
        memcpy(data + 8 + js_size, rtc_data->Payload()->Data(), rtc_data->Payload()->Size());

        if (transport_) {
            transport_->Send(packet);
        }
    }

    srs_trace("QnRtcManager thread quit... \n");
    return err;
}

srs_error_t QnRtcManager::OnProducerData(const QnDataPacket_SharePtr& packet)
{
    srs_error_t err = srs_success;

    // big endian
    uint8_t* data = (uint8_t*)packet->Data();

    uint32_t total_size = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    uint16_t js_size = (data[4] << 24) | (data[5] << 16) | (data[6] << 8)  | data[7];

    QnRtcData_SharePtr rtc_data = std::make_shared<QnRtcData>();

    json& js = rtc_data->Head();
    std::string head((char*)data + 8, js_size);
    // srs_trace("recv js:%s\n", head.c_str());
    js = json::parse(head);

    if (!json_have(js, "unique_id") || !json_have(js, "stream_url")) {
        srs_error("producer data no unique_id or stream_url, error");
        return err;
    }

    uint64_t unique_id;
    json_do_default(unique_id, js["unique_id"], 0);
    if (unique_id != recv_unique_id_ + 1) {
        srs_warn("unique id jumped, %lld --> %lld\n", recv_unique_id_, unique_id);
    }
    recv_unique_id_ = unique_id;

    std::string stream_url;
    json_do_default(stream_url, js["stream_url"], "^&unknow");
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

    char* payload = (char*)data + 8 + js_size;
    uint32_t payload_size = total_size - js_size - 8;
    QnDataPacket_SharePtr payload_packet = std::make_shared<QnDataPacket>(payload_size);
    memcpy(payload_packet->Data(), payload, payload_size);
    // srs_trace("total_size:%u, jsoffset:%u, jssize:%d, payload size:%u\n", total_size, js_offset, js_size, payload_size);
    rtc_data->SetPayload(payload_packet);
    rtc_data->SetStreamUrl(stream_url);
    rtc_data->SetType(en_RtcDataType_Media);

    req_stream->producer->on_data(rtc_data);

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
    // TODO
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

srs_error_t QnRtcManager::on_timer(srs_utime_t interval)
{
    srs_trace2("QNDUMP", "<== request streams:%u", map_req_streams_.size());
    for (auto it = map_req_streams_.begin(); it != map_req_streams_.end(); it++) {
        QnReqStream* req_stream = it->second;
        srs_trace2("QNDUMP", "[ %s, enable:%d, needs:%d ]", qn_get_origin_stream(it->first).c_str(), 
                    req_stream->enable, req_stream->users.size());
        if (req_stream->producer) {
            req_stream->producer->Dump();
        }
    }

    srs_trace2("QNDUMP", "==> publish streams:%u", map_consumers_.size());
    for (auto it = map_consumers_.begin(); it != map_consumers_.end(); it++) {
        srs_trace2("QNDUMP", "[ %s ]", it->first.c_str());
        it->second->Dump();
    }

    srs_trace2("QNDUMP", "==> consumer packets2send:%u", vec_consumer_data_.size());
    return srs_success;
}


QnTransport::QnTransport(const std::string& name, const TransRecvCbType& callback)
{
    name_ = name;
    recv_callback_ = callback;
}

QnTransport::~QnTransport()
{
}

uint32_t QnTransport::MakeCheckSum32(uint8_t* pdata, uint32_t dataLen)
{
    uint32_t len;
    uint32_t chk = 0;

    // 按照小端的方式计算4个字节的checksum
    len = dataLen;
    while (len >= 4)
    {
        chk +=   (uint32_t)pdata[0];                //     ----          ----         ----       pdata[0]
        chk +=   ((uint32_t)pdata[1] << 8);         //     ----          ----        pdata[1]     ----
        chk +=   ((uint32_t)pdata[2] << 16);        //     ----         pdata[2]      ----        ----
        chk +=   ((uint32_t)pdata[3] << 24);        //     pdata[3]      ----         ----        ----

        pdata += 4;
        len -= 4;
    }

    if (len > 0)
    {
        if (len == 1)
        {
            chk +=   (uint32_t)pdata[0];
        }
        else if (len == 2)
        {
            chk +=   (uint32_t)pdata[0];
            chk +=   ((uint32_t)pdata[1] << 8);
        }
        else
        {
            chk +=   (uint32_t)pdata[0];
            chk +=   ((uint32_t)pdata[1] << 8);
            chk +=   ((uint32_t)pdata[2] << 16);
        }
    }

    chk = ~chk;
    chk++;

    return chk;
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

uint32_t QnLoopTransport::GetResverdSize()
{
    return 0;
}

srs_error_t QnLoopTransport::Send(const QnDataPacket_SharePtr& packet)
{
    // srs_trace("LoopTransport send %u bytes\n", packet->Size());
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

        // srs_trace("packets wait transport, %d\n", vec_packets_.size());
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


QnSocketPairTransport::QnSocketPairTransport(const std::string& name, const TransRecvCbType& callback) :
                                                QnTransport(name, callback)
{
    max_buf_size_ = 2048;
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, fds_) < 0) {
        srs_error("error %d on socketpair\n", errno);
    }

    auto f = [&]() {
        srs_trace("trans thread start");
        char* buffer = new char[max_buf_size_];
        while (true) {
            auto read_size = read(fds_[1], buffer, max_buf_size_);
            if (read_size > 0) {
                write(fds_[1], buffer, read_size);
            }
        }
        srs_trace("trans thread quit...");
    };

    trans_thread_ = new std::thread(f);
    trans_thread_->detach();

    rwfd_ = srs_netfd_open_socket(fds_[0]);
    packet_cond_ = srs_cond_new();
    trd_ = new SrsSTCoroutine("sockpair-transport", this);
    trd_->start();
}

QnSocketPairTransport::~QnSocketPairTransport()
{

}

uint32_t QnSocketPairTransport::GetResverdSize()
{
    return 0;
}

srs_error_t QnSocketPairTransport::Send(const QnDataPacket_SharePtr& packet)
{
    srs_error_t err = srs_success;

    ssize_t write_size = srs_write(rwfd_, packet->Data(), packet->Size(), 2000000);
    if (write_size < 0) {
        srs_trace("st write error %s(%d)", strerror(errno), errno);
        return srs_error_wrap(err, "st_write error");
    }

    if (write_size < packet->Size()) {
        srs_trace("st write timeout %s(%d)", strerror(errno), errno);
        return srs_error_wrap(err, "st_write timeout");
    }

    return err;
}

srs_error_t QnSocketPairTransport::cycle()
{
    srs_error_t err = srs_success;
    srs_trace("QnSocketPairTransport thread running \n");

    char* buffer = NULL;

    while (true) {
        if ((err = trd_->pull()) != srs_success) {
            return srs_error_wrap(err, "buffer cache");
        }

        if (!buffer) {
            buffer = new char[max_buf_size_];
            if (!buffer) {
                srs_error("new buffer error");
                srs_usleep(10000);
                continue;
            }
        }        

        ssize_t read_size = srs_read(rwfd_, buffer, max_buf_size_, 5000000);
        if (read_size < 0) {
            srs_trace("st read error %s(%d)", strerror(errno), errno);
            continue;
        }

        if (read_size == 0) {
            continue;
        }

        if (recv_callback_) {
            QnDataPacket_SharePtr packet = std::make_shared<QnDataPacket>(buffer, read_size);
            buffer = NULL;
            recv_callback_(name_, packet);
        }
    }

    if (buffer) {
        delete[] buffer;
    }

    srs_trace("QnSocketPairTransport thread quit... \n");
    return err;
}
