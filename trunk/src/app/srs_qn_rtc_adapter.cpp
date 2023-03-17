#include <algorithm>
#include <exception>
#include <unistd.h>
#include <srs_qn_rtc_adapter.hpp>
#include <srs_app_hybrid.hpp>
#include <srs_app_server.hpp>
#include <srs_app_rtc_source.hpp>
#include <libcurl/curl.h>

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


const std::string PLAY_STREAM_TAG = "--qnplaystream11";

// mb20230308 播放stream自动加上不太可能被使用的后缀，而且
// 最好是判断下发布的stream名字，不能包含这个后缀
std::string qn_get_play_stream(const std::string& stream)
{
    std::string::size_type pos;
    if ((pos = stream.find(PLAY_STREAM_TAG)) != std::string::npos) {
        return std::string(stream);
    }

    return stream + PLAY_STREAM_TAG;
}

std::string qn_get_origin_stream(const std::string& stream)
{
    std::string::size_type pos;
    if ((pos = stream.find(PLAY_STREAM_TAG)) == std::string::npos) {
        return std::string(stream);
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
    en_RtcDataType_PublishStream,       // 发布新的数据流
    en_RtcDataType_UnPublishStream,     // 停止发布数据流
    en_RtcDataType_RequestStream,       // 请求数据流
    en_RtcDataType_StopStream,          // 停止请求数据流
    en_RtcDataType_PublishStreamPlay,
    en_RtcDataType_UnPublishStreamPlay,

    en_RtcDataType_butt
};

const std::string ID = "id";
const std::string PACKET_ID = "packet_id";
const std::string ASTIME = "astime";
const std::string MTYPE = "mtype";
const std::string PAYLOAD_TYPE = "pt";
const std::string MARK_BIT = "mark";
const std::string KEY_FRAME = "key";

//| total size(4bytes) | json size(4bytes) | data |
const uint32_t JSON_IN_HEAD_SIZE = 8;

//| total size(4bytes) | data |
const uint32_t DATA_ONLY_HEAD_SIZE = 4;

static void WriteUint32BE(uint8_t* data, uint32_t value)
{
    data[0] = ((value >> 24) & 0xff);
    data[1] = ((value >> 16) & 0xff);
    data[2] = ((value >> 8) & 0xff);
    data[3] = (value & 0xff);
}

static uint32_t ReadUint32BE(uint8_t* data)
{
    uint32_t value = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    return value;
}


// 带json的转为不带json的
uint8_t* Msg2RtpExt(const QnDataPacket_SharePtr& packet, size_t& size)
{
    // 发送格式只能是4字节的长度 + rtp包，把json格式的数据写入rtp扩展
    // big endian
    uint8_t* data = (uint8_t*)packet->Data();

    uint32_t total_size = ReadUint32BE(data);
    uint32_t js_size = ReadUint32BE(data + 4);
    srs_assert(total_size == packet->Size());

    json js;
    std::string head((char*)data + JSON_IN_HEAD_SIZE, js_size);
    js = json::parse(head);

    std::string source_id;
    json_do_default(source_id, js[ID], "**unknow");

    uint64_t unique_id;
    json_do_default(unique_id, js[PACKET_ID], 0);

    std::string type;
    json_do_default(type, js[MTYPE], "unknow");

    int64_t astime;
    json_do_default(astime, js[ASTIME], 0);

    uint32_t rtp_size = total_size - JSON_IN_HEAD_SIZE - js_size;
    uint8_t* rtp_data = data + JSON_IN_HEAD_SIZE + js_size;

    // 找到扩展头的位置
    int16_t ext_size = 0;
    bool have_ext = rtp_data[0] & 0x10;
    int16_t rel_ext_size = 0;
    uint8_t cc = rtp_data[0] & 0x0f;
    uint32_t size_to_cc = 12 + 4 * cc;
    uint8_t* ext_data = rtp_data + size_to_cc;

    if (have_ext) {
        srs_assert(ext_data[0] == 0xBE);
        srs_assert(ext_data[1] == 0xDE);
        ext_size = (ext_data[2] << 8) | ext_data[3];
        ext_size *= 4;
        uint8_t* p = ext_data + 4;
        while ((rel_ext_size < ext_size) && (*p != 0)) {
            uint8_t t = p[0] & 0xf0;
            uint8_t s = p[0] & 0x0f;
            rel_ext_size += (s + 1 + 1);
            p += (s + 1 + 1);
            srs_trace("ext_type:%hhu, size:%hhu", t, s + 1);
        }

        srs_assert(rel_ext_size <= ext_size);
    }

    uint32_t new_ext_size = rel_ext_size;
    new_ext_size += source_id.size();
    new_ext_size += 1;
    new_ext_size += sizeof(unique_id);
    new_ext_size += 1;
    new_ext_size += type.size();
    new_ext_size += 1;
    new_ext_size += sizeof(astime);
    new_ext_size += 1;

    uint32_t new_pad_count = (new_ext_size % 4 == 0) ? 0 : (4 - new_ext_size % 4);
    new_ext_size += new_pad_count;

    uint32_t payload_size = rtp_size - size_to_cc;
    uint8_t* payload_addr = rtp_data + size_to_cc;
    if (have_ext) {
        payload_size -= (4 + ext_size);
        payload_addr += (4 + ext_size);
    }

    uint32_t new_total_size = DATA_ONLY_HEAD_SIZE + size_to_cc + 4 + new_ext_size + payload_size;

    uint8_t* data_new = new uint8_t[new_total_size];
    srs_assert(data_new);

    uint8_t* data_write = data_new + DATA_ONLY_HEAD_SIZE;
    memcpy(data_write, rtp_data, size_to_cc);
    data_write[0] |= 0x10;      // 有扩展头标识
    data_write += size_to_cc;

    data_write[0] = 0xBE;
    data_write[1] = 0xDE;
    data_write[2] = ((new_ext_size / 4) >> 8) & 0xff;
    data_write[3] = ((new_ext_size / 4) & 0xff);
    data_write += 4;

    if (rel_ext_size > 0) {
        memcpy(data_write, ext_data + 4, rel_ext_size);
        data_write += rel_ext_size;
    }

    // 新加扩展
    data_write[0] = 0xc0 | (source_id.size() - 1);     // 扩展类型12
    data_write++;
    memcpy(data_write, source_id.c_str(), source_id.size());
    data_write += source_id.size();

    data_write[0] = 0x10 | (sizeof(unique_id) - 1);     // 扩展类型13
    data_write++;
    *(uint64_t*)data_write = unique_id;
    data_write += sizeof(unique_id);

    data_write[0] = 0xe0 | (sizeof(astime) - 1);        // 扩展类型14
    data_write++;
    *(int64_t*)data_write = astime;
    data_write += sizeof(astime);

    data_write[0] = 0xf0 | (type.size() - 1);           // 扩展类型15
    data_write++;
    memcpy(data_write, type.c_str(), type.size());
    data_write += type.size();

    if (new_pad_count > 0) {
        memset(data_write, 0, new_pad_count);
    }

    data_write = data_new + DATA_ONLY_HEAD_SIZE + size_to_cc + 4 + new_ext_size;
    memcpy(data_write, payload_addr, payload_size);

    // write total_size
    WriteUint32BE(data_new, new_total_size);

    size = new_total_size;
    return data_new;
}

// 不带json的转为带json的
QnDataPacket_SharePtr MsgFromRtpExt(const std::string& stream_url, uint8_t* rdt, size_t size)
{
    uint32_t total_size_old = (rdt[0] << 24) | (rdt[1] << 16) | (rdt[2] << 8) | rdt[3];
    srs_assert(total_size_old == size);

    uint32_t rtp_size = total_size_old - DATA_ONLY_HEAD_SIZE;
    uint8_t* rtp_data = rdt + DATA_ONLY_HEAD_SIZE;

    json js;
    js["stream_url"] = stream_url;

    std::string source_id;
    uint64_t unique_id;
    std::string type;
    int64_t astime;

    // 找到扩展头的位置
    int16_t ext_size = 0;
    bool have_ext = rtp_data[0] & 0x10;
    srs_assert(have_ext);
    int16_t rel_ext_size = 0;
    uint8_t cc = rtp_data[0] & 0x0f;
    uint32_t size_to_cc = 12 + 4 * cc;
    uint8_t* ext_data = rtp_data + size_to_cc;

    if (have_ext) {
        srs_assert(ext_data[0] == 0xBE);
        srs_assert(ext_data[1] == 0xDE);
        ext_size = (ext_data[2] << 8) | ext_data[3];
        ext_size *= 4;
        uint8_t* p = ext_data + 4;
        while ((rel_ext_size < ext_size) && (*p != 0)) {
            uint8_t t = p[0] & 0xf0;
            uint8_t s = p[0] & 0x0f;
            // srs_trace("ext_type:%hhu, size:%hhu, %hd,%hd", t, s + 1, rel_ext_size, ext_size);
            if (t == 0xf0) {
                type = std::string((char*)(p + 1), s + 1);
                js[MTYPE] = type;
                // srs_trace("type:%s", type.c_str());
            } else if (t == 0xe0) {
                astime = *(int64_t*)(p + 1);
                js[ASTIME] = astime;
                // srs_trace("astime:%lld", astime);
            } else if (t == 0x10) {
                unique_id = *(uint64_t*)(p + 1);
                js[PACKET_ID] = unique_id;
                // srs_trace("unique_id:%llu", unique_id);
            } else if (t == 0xc0) {
                source_id = std::string((char*)(p + 1), s + 1);
                js[ID] = source_id;
                // srs_trace("source_id:%s", source_id.c_str());
            }

            rel_ext_size += (s + 1 + 1);
            p += (s + 1 + 1);
        }

        srs_assert(rel_ext_size <= ext_size);
    }

    std::string head = js.dump();
    // srs_trace("send js:%s\n", head.c_str());
    uint32_t js_size = head.size();
    // 前面8字节固定，不能改动
    /*************************************************************
     | total size(4bytes) | json size(4bytes) | json | raw data | 
    **************************************************************/
    uint32_t total_size = JSON_IN_HEAD_SIZE + js_size + rtp_size;
    QnDataPacket_SharePtr packet = std::make_shared<QnDataPacket>(total_size);

    // big endian
    uint8_t* data = (uint8_t*)packet->Data();

    // write total_size
    WriteUint32BE(data, total_size);

    // json size
    WriteUint32BE(data + 4, js_size);
    
    // json data
    memcpy(data + JSON_IN_HEAD_SIZE, head.c_str(), js_size);

    // raw payload data
    memcpy(data + JSON_IN_HEAD_SIZE + js_size, rtp_data, rtp_size);

    return packet;
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
    if (data_) {
        if (!pfree_) {
            delete[] data_;
        } else {
            pfree_(data_, size_);
        }
    }
    
    data_ = NULL;
    size_ = 0;
}

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

    QnRtcManager::Instance()->AddConsumer(this);
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

srs_error_t QnRtcConsumer::on_publish()
{
    QnRtcManager::Instance()->StartPublish(stream_url_);
    return srs_success;
}

srs_error_t QnRtcConsumer::on_unpublish()
{
    QnRtcManager::Instance()->StopPublish(stream_url_);
    return srs_success;
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

    // if (!pkt->is_audio()) {
    //     js[PAYLOAD_TYPE] = pkt->payload_type();
    //     js[KEY_FRAME] = pkt->is_keyframe() ? 1 : 0;
    //     js[MARK_BIT] = pkt->header.get_marker() ? 1 : 0;
    // }

    QnRtcManager::Instance()->OnRtcData(rtc_data);

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
    auto recv_callback = [&](const std::string& stream_url, int32_t type, const QnDataPacket_SharePtr& packet) {
        // srs_trace("recv data from %s, size:%u\n", flag.c_str(), packet->Size());
        OnProducerData(stream_url, type, packet);
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
                // srs_assert(req_stream->producer);
                // if (req_stream->producer) {
                //     req_stream->producer->on_publish();
                // }
                req_stream->enable = true;
            }
            StartSubscribe(stream_url);
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

    // req_stream->producer->on_publish();
    req_stream->published = false;
    req_stream->enable = true;
    StartSubscribe(stream_url);

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
                StopSubscibe(stream_url);
                // if (req_stream->producer){
                //     req_stream->producer->on_unpublish();
                // }
            }
        }
    }

    return srs_success;
}

srs_error_t QnRtcManager::AddConsumer(QnRtcConsumer* consumer)
{
    std::string stream_url = consumer->source_stream_url();
    auto it = map_pub_streams_.find(stream_url);
    if (it == map_pub_streams_.end()) {
        QnPubStream* pub_stream = new QnPubStream;
        srs_assert(pub_stream);
        pub_stream->published = false;
        pub_stream->consumer = consumer;
        map_pub_streams_[stream_url] = pub_stream;
    }

    return srs_success;
}

void QnRtcManager::StartPublish(const std::string& stream_url)
{
    auto it = map_pub_streams_.find(stream_url);
    if (it == map_pub_streams_.end()) {
        srs_error("publish stream not exist, error", stream_url.c_str());
        return;
    }

    it->second->published = true;

    QnRtcData_SharePtr rtc_data = std::make_shared<QnRtcData>();
    rtc_data->SetStreamUrl(stream_url);
    rtc_data->SetType(en_RtcDataType_PublishStream);
    OnRtcData(rtc_data);
}

void QnRtcManager::StopPublish(const std::string& stream_url)
{
    auto it = map_pub_streams_.find(stream_url);
    if (it == map_pub_streams_.end()) {
        srs_error("unpublish stream not exist, error", stream_url.c_str());
        return;
    }

    it->second->published = false;

    QnRtcData_SharePtr rtc_data = std::make_shared<QnRtcData>();
    rtc_data->SetStreamUrl(stream_url);
    rtc_data->SetType(en_RtcDataType_UnPublishStream);
    OnRtcData(rtc_data);
}

srs_error_t QnRtcManager::OnRtcData(const QnRtcData_SharePtr& rtc_data)
{
    vec_consumer_data_.push_back(rtc_data);
    srs_cond_signal(consumer_data_cond_);
    return srs_success;
}

void QnRtcManager::StartSubscribe(const std::string& stream_url)
{
    QnRtcData_SharePtr rtc_data = std::make_shared<QnRtcData>();
    rtc_data->SetStreamUrl(stream_url);
    rtc_data->SetType(en_RtcDataType_RequestStream);
    OnRtcData(rtc_data);
}

void QnRtcManager::StopSubscibe(const std::string& stream_url)
{
    QnRtcData_SharePtr rtc_data = std::make_shared<QnRtcData>();
    rtc_data->SetStreamUrl(stream_url);
    rtc_data->SetType(en_RtcDataType_StopStream);
    OnRtcData(rtc_data);
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
    
        // 非媒体数据的控制消息
        if (rtc_data->Type() != en_RtcDataType_Media) {        
            if (transport_) {
                transport_->Send(rtc_data->StreamUrl(), rtc_data->Type(), nullptr);
            }
            continue;
        }

        json& js = rtc_data->Head();
        // 添加系列号以判断是否数据有丢失
        js["stream_url"] = rtc_data->StreamUrl();

        std::string head = js.dump();
        // srs_trace("send js:%s\n", head.c_str());
        uint32_t js_size = head.size();
        // 前面8字节固定，不能改动
        /*************************************************************
         | total size(4bytes) | json size(4bytes) | json | raw data | 
        **************************************************************/
        uint32_t total_size = JSON_IN_HEAD_SIZE + js_size + rtc_data->Payload()->Size();
        QnDataPacket_SharePtr packet = std::make_shared<QnDataPacket>(total_size);

        // big endian
        uint8_t* data = (uint8_t*)packet->Data();

        // write total_size
        WriteUint32BE(data, total_size);

        // json size
        WriteUint32BE(data + 4, js_size);
        
        // json data
        memcpy(data + JSON_IN_HEAD_SIZE, head.c_str(), js_size);

        // raw payload data
        memcpy(data + JSON_IN_HEAD_SIZE + js_size, rtc_data->Payload()->Data(), rtc_data->Payload()->Size());

        if (transport_) {
            transport_->Send(rtc_data->StreamUrl(), rtc_data->Type(), packet);
        }
    }

    srs_trace("QnRtcManager thread quit... \n");
    return err;
}

srs_error_t QnRtcManager::OnProducerData(const std::string& stream_url, int32_t type, const QnDataPacket_SharePtr& packet)
{
    srs_error_t err = srs_success;

    auto it = map_req_streams_.find(stream_url);
    if (it == map_req_streams_.end()) {
        // srs_error("request for stream %s not found", stream_url.c_str());
        return err;
    }

    QnReqStream* req_stream = it->second;

    if (!req_stream->producer) {
        srs_warn("producer not exist, stream:%s\n", stream_url.c_str());
        return err;
    }

    if (type == en_RtcDataType_UnPublishStreamPlay) { 
        if (req_stream->published) {
            req_stream->producer->on_unpublish();
            req_stream->published = false;
        }
        return err;
    }

    if (!req_stream->enable) {
        // srs_error("request for stream %s not enable", stream_url.c_str());
        return err;
    }

    if (type == en_RtcDataType_PublishStreamPlay) {
        if (!req_stream->published) {
            req_stream->producer->on_publish();
            req_stream->published = true;
        }
        return err;
    }

    if (!req_stream->published) {
        req_stream->producer->on_publish();
        req_stream->published = true;
    }

    // big endian
    uint8_t* data = (uint8_t*)packet->Data();

    uint32_t total_size = ReadUint32BE(data);
    uint32_t js_size = ReadUint32BE(data + 4);

    QnRtcData_SharePtr rtc_data = std::make_shared<QnRtcData>();

    json& js = rtc_data->Head();
    std::string head((char*)data + JSON_IN_HEAD_SIZE, js_size);
    // srs_trace("recv js:%s\n", head.c_str());
    js = json::parse(head);

    // if (!json_have(js, "stream_url")) {
    //     srs_error("producer data no unique_id or stream_url, error");
    //     return err;
    // }
    // std::string stream_url;
    // json_do_default(stream_url, js["stream_url"], "^&unknow");
    // stream_url = qn_get_play_stream(stream_url);

    char* payload = (char*)data + JSON_IN_HEAD_SIZE + js_size;
    uint32_t payload_size = total_size - js_size - JSON_IN_HEAD_SIZE;
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
    srs_trace("new SrsRtmpFromRtcBridge for %s start", req->get_stream_url().c_str());
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
    rtmp->set_cache(true);

    SrsRtmpFromRtcBridge *bridge = new SrsRtmpFromRtcBridge(rtmp);
    if ((err = bridge->initialize(req)) != srs_success) {
        srs_freep(bridge);
        return srs_error_wrap(err, "create bridge");
    }

    source->set_bridge(bridge);
    producer = new QnRtcProducer(source);

    srs_trace("new SrsRtmpFromRtcBridge for %s succ...", req->get_stream_url().c_str());
    return srs_success;
}

srs_error_t QnRtcManager::on_timer(srs_utime_t interval)
{
    srs_trace2("QNDUMP", "<== request streams:%u", map_req_streams_.size());
    for (auto it = map_req_streams_.begin(); it != map_req_streams_.end(); it++) {
        QnReqStream* req_stream = it->second;
        srs_trace2("QNDUMP", "[ %s, published:%d, enable:%d, needs:%d ]", qn_get_origin_stream(it->first).c_str(), 
                    req_stream->published, req_stream->enable, req_stream->users.size());
        if (req_stream->producer) {
            req_stream->producer->Dump();
        }
    }

    srs_trace2("QNDUMP", "==> publish streams:%u", map_pub_streams_.size());
    for (auto it = map_pub_streams_.begin(); it != map_pub_streams_.end(); it++) {
        srs_trace2("QNDUMP", "[ %s, published:%d ]", it->first.c_str(), it->second->published);
        it->second->consumer->Dump();
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

srs_error_t QnLoopTransport::Send(const std::string& stream_url, int32_t type, const QnDataPacket_SharePtr& packet)
{
    // srs_trace("LoopTransport send %u bytes\n", packet->Size());
    TransMsg* msg = new TransMsg;
    srs_assert(msg);

    msg->stream_url = stream_url;
    msg->type = type;
    msg->packet = packet;

    vec_packets_.push_back(msg);
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

        TransMsg* msg = *it;
        vec_packets_.erase(it);

        if (recv_callback_) {
            recv_callback_(msg->stream_url, msg->type, msg->packet);
        }

        delete msg;
    }

    srs_trace("QnLoopTransport thread quit... \n");
    return err;
}


///////////////////////////////////////////////////////////////////////////////////

QnSocketPairTransport::QnSocketPairTransport(const std::string& name, const TransRecvCbType& callback) :
                                                QnTransport(name, callback)
{
    pthread_mutex_init(&wt_mutex_, NULL);

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, fds_) < 0) {
        srs_error("error %d on socketpair\n", errno);
    }

    auto f = [&]() {
        thread_process();
    };

    trans_thread_ = new std::thread(f);
    trans_thread_->detach();

    rwfd_ = srs_netfd_open_socket(fds_[0]);
    packet_cond_ = srs_cond_new();
    trd_ = new SrsSTCoroutine("sockpair-transport", this);
    trd_->start();

    char const *val = getenv("GATE_SERVER");
    if (val) {
        gate_server_ = std::string(val);
        srs_trace("gate server: %s", gate_server_.c_str());
    }
}

QnSocketPairTransport::~QnSocketPairTransport()
{

}

uint32_t QnSocketPairTransport::GetResverdSize()
{
    return 0;
}

srs_error_t QnSocketPairTransport::Send(const std::string& stream_url, int32_t type, const QnDataPacket_SharePtr& packet)
{
    srs_error_t err = srs_success;

    TransMsg* msg = new TransMsg;
    srs_assert(msg);

    msg->stream_url = stream_url;
    msg->type = type;
    msg->packet = packet;

    ssize_t write_size = srs_write(rwfd_, &msg, sizeof(msg), 2000000);
    if (write_size < 0) {
        srs_trace("st write error %s(%d)", strerror(errno), errno);
        return srs_error_wrap(err, "st_write error");
    }

    return err;
}

srs_error_t QnSocketPairTransport::cycle()
{
    srs_error_t err = srs_success;
    srs_trace("QnSocketPairTransport thread running \n");

    while (true) {
        if ((err = trd_->pull()) != srs_success) {
            return srs_error_wrap(err, "buffer cache");
        }

        TransMsg* msg;
        ssize_t read_size = srs_read(rwfd_, &msg, sizeof(msg), 5000000);
        if (read_size < 0) {
            srs_trace("st read error %s(%d)", strerror(errno), errno);
            continue;
        }

        if (read_size != sizeof(msg)) {
            srs_error("srs_read size error, %u != %u", read_size, sizeof(msg));
            continue;
        }

        srs_assert(msg);
        // srs_assert(msg->type == en_RtcDataType_Media);

        if (recv_callback_) {
            recv_callback_(msg->stream_url, msg->type, msg->packet);
        }

        delete msg;
    }

    srs_trace("QnSocketPairTransport thread quit... \n");
    return err;
}

void QnSocketPairTransport::thread_process()
{
    srs_trace("trans thread start");

    while (true) {
        TransMsg* msg;
        auto read_size = read(fds_[1], &msg, sizeof(msg));
        if (read_size <= 0) {
            continue;
        }

        if (read_size != sizeof(msg)) {
            srs_error("read size error, %u != %u", read_size, sizeof(msg));
            continue;
        }

        // 没有服务器，则自环
        if (gate_server_.empty()) {
            if (msg->type == en_RtcDataType_Media) {
                write(fds_[1], &msg, read_size);
            } else {
                delete msg;
            }
            continue;
        }

        if (msg->type == en_RtcDataType_Media || 
            msg->type == en_RtcDataType_PublishStream || 
            msg->type == en_RtcDataType_UnPublishStream) {
            deal_publish_msg(msg);
        } else 
        if (msg->type == en_RtcDataType_RequestStream || 
            msg->type == en_RtcDataType_StopStream) {
            deal_request_msg(msg);
        } else {
            srs_error("unknow msg type:%d", msg->type);
            delete msg;
        }
    }

    srs_trace("trans thread quit...");
}

void QnSocketPairTransport::deal_publish_msg(TransMsg* msg)
{
    const std::string& stream_url = msg->stream_url;

    bool new_sender = false;
    auto it = map_stream_senders_.find(stream_url);
    if (it == map_stream_senders_.end()) {
        if (msg->type == en_RtcDataType_UnPublishStream) {
            return;
        }

        StreamSender* stream_sender = new HttpStreamSender(gate_server_, stream_url);
        srs_assert(stream_sender);
        new_sender = true;
        map_stream_senders_[stream_url] = stream_sender;
        it = map_stream_senders_.find(stream_url);
    }

    StreamSender* stream_sender = it->second;
    srs_assert(stream_sender);

    if ((msg->type == en_RtcDataType_PublishStream) || new_sender) {
        srs_trace("start sender for publish %s", stream_url.c_str());
        stream_sender->Start();
        delete msg;
        return;
    }

    if (msg->type == en_RtcDataType_Media) {
        stream_sender->Send(msg);
        return;
    }

    if (msg->type == en_RtcDataType_UnPublishStream) {
        srs_trace("stop sender for publish %s", stream_url.c_str());
        stream_sender->Stop();
        delete msg;
        return;
    }
}

void QnSocketPairTransport::deal_request_msg(TransMsg* msg)
{
    const std::string& stream_url = msg->stream_url;

    auto it = map_stream_receivers_.find(stream_url);
    if (it == map_stream_receivers_.end()) {
        if (msg->type == en_RtcDataType_StopStream) {
            return;
        }

        auto f = [&](const std::string& flag, TransMsg* msg) {
            if (map_stream_receivers_.find(flag) == map_stream_receivers_.end()) {
                srs_error("stream %s not exist\n", flag.c_str());
                delete msg;
            } else {
                pthread_mutex_lock(&wt_mutex_);
                write(fds_[1], &msg, sizeof(msg));
                pthread_mutex_unlock(&wt_mutex_);
            }
        };

        StreamReceiver* stream_receiver = new HttpStreamReceiver(gate_server_, stream_url, f);
        srs_assert(stream_receiver);
        map_stream_receivers_[stream_url] = stream_receiver;
        it = map_stream_receivers_.find(stream_url);
    }

    StreamReceiver* stream_receiver = it->second;
    srs_assert(stream_receiver);

    if (msg->type == en_RtcDataType_RequestStream) {
        stream_receiver->Start();
        delete msg;
        return;
    }

    if (msg->type == en_RtcDataType_StopStream) {
        stream_receiver->Stop();
        delete msg;
        return;
    }
}


HttpStreamSender::HttpStreamSender(const std::string gate_server, const std::string& stream_url) : 
                                    StreamSender(gate_server, stream_url)
{
    pthread_mutex_init(&mutex_, NULL);
    started_ = false;
    wait_quit_ = false;
    thread_ = NULL;
    last_data_ = NULL;
    last_data_size_ = 0;
    last_data_offset_ = 0;
    // session_ = (uint64_t)srs_update_system_time();
}

HttpStreamSender::~HttpStreamSender()
{

}

srs_error_t HttpStreamSender::Start()
{
    srs_error_t err = srs_success;
    if (started_ || wait_quit_) {
        srs_trace("stream sender already started, %s\n", stream_url_.c_str());
        return err;
    }

    srs_trace("start stream sender, %s\n", stream_url_.c_str());
    auto f = [&]() {
        SendProc();
        wait_quit_ = false;
        started_ = false;
    };

    wait_quit_ = false;
    started_ = true;
    if (thread_) {
        delete thread_;
    }
    thread_ = new std::thread(f);
    thread_->detach();
    return err;
}

void HttpStreamSender::Stop()
{
    srs_trace("stop stream sender, %s\n", stream_url_.c_str());
    wait_quit_ = true;
    started_ = false;
}

srs_error_t HttpStreamSender::Send(TransMsg* msg)
{
    srs_error_t err = srs_success;
    if (!started_) {
        delete msg;
        return err;
    }

    if (msg->type != en_RtcDataType_Media) {
        srs_trace("do not send msg of type %d", msg->type);
        return err;
    }

    pthread_mutex_lock(&mutex_);
    if (vec_msgs_.size() >= 200) {
        srs_error("too much(%d) packets wait to send, %s", vec_msgs_.size(), stream_url_.c_str());
        delete msg;
        pthread_mutex_unlock(&mutex_);
        return err;
    }

    vec_msgs_.push_back(msg);
    pthread_mutex_unlock(&mutex_);
    return err;
}

static size_t StreamSenderReadCallback(char *dest, size_t size, size_t nmemb, void *userp)
{
    return ((HttpStreamSender*)userp)->SendMoreCallback(dest, size, nmemb);
}

size_t HttpStreamSender::SendMoreCallback(char *dest, size_t size, size_t nmemb)
{
    if (first_data_) {
        first_data_ = false;
        srs_trace("interval of send start and read callback:%lld", srs_update_system_time() - tick_start_);
    }

    if (wait_quit_) {
        srs_trace("return for quit send %s", stream_url_.c_str());
        return CURL_READFUNC_ABORT;
    }

    size_t buffer_size = size * nmemb;
    if (buffer_size <= 0) {
        srs_trace("callback size %d, return for quit send %s", buffer_size, stream_url_.c_str());
        return CURL_WRITEFUNC_ERROR;
    }

    if (!last_data_) {
        pthread_mutex_lock(&mutex_);
        while (vec_msgs_.empty()) {
            pthread_mutex_unlock(&mutex_);
            usleep(2000);
            if (wait_quit_) {
                srs_trace("return for quit send %s", stream_url_.c_str());
                return CURL_READFUNC_ABORT ;
            }
            pthread_mutex_lock(&mutex_);
        }

        auto it = vec_msgs_.begin();
        TransMsg* msg = *it;
        vec_msgs_.erase(it);
        pthread_mutex_unlock(&mutex_);

        last_data_ = Msg2RtpExt(msg->packet, last_data_size_);
        last_data_offset_ = 0;

        // 注意：服务器的格式要求，前4字节的total_size不包括前4个字节本身
        WriteUint32BE(last_data_, last_data_size_ - 4);

        delete msg;
    }

    if (last_data_size_ < buffer_size) {
        size_t size = last_data_size_;
        memcpy(dest, last_data_ + last_data_offset_, size);
        delete[] last_data_;
        last_data_ = NULL;
        last_data_size_ = 0;
        last_data_offset_ = 0;
        // srs_trace("SendMoreCallback dest:%p, need:%d, write:%d", dest, buffer_size, size);
        return size;
    }

    memcpy(dest, last_data_ + last_data_offset_, buffer_size);
    last_data_size_ -= buffer_size;
    last_data_offset_ += buffer_size;

    // srs_trace("SendMoreCallback dest:%p, need:%d, write:%d", dest, buffer_size, buffer_size);
    return buffer_size;
}

static bool CurlGlobalInit()
{
    static bool inited = false;
    if (inited) {
        return true;
    }

    CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
    if(res != CURLE_OK) {
        srs_error("curl_global_init failed: %s\n", curl_easy_strerror(res));
        return false;
    }

    inited = true;
    return true;
}

void HttpStreamSender::SendProc()
{
    srs_trace("thread for stream sender start, %s", stream_url_.c_str());

    session_ = (uint64_t)srs_update_system_time();
    tick_start_ = 0;

    for (;;) {

        if (!started_) {
            break;
        }

        int64_t t0 = srs_update_system_time();
        if (t0 - tick_start_ < 5 * SRS_UTIME_SECONDS) {
            usleep(20000);
            continue;
        }

        tick_start_ = srs_update_system_time();
        first_data_ = true;

        CURL *curl;
        CURLcode res;

        if (!CurlGlobalInit()) {
            break;
        }

        curl = curl_easy_init();
        if (!curl) {
            srs_error("curl_easy_init error");
            continue;
        }

        std::string url = "http://" + gate_server_ + stream_url_;
        srs_trace("stream send to %s\n", url.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, StreamSenderReadCallback);
        curl_easy_setopt(curl, CURLOPT_READDATA, this);
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

        struct curl_slist *chunk = NULL;
        std::string session_id = "x-miku-session-id: " + std::to_string(session_);
        // chunk = curl_slist_append(chunk, "Transfer-Encoding: chunked");
        chunk = curl_slist_append(chunk, "Expect: 100-continue");
        chunk = curl_slist_append(chunk, session_id.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

        res = curl_easy_perform(curl);
        if(res != CURLE_OK) {
            srs_error("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        }

        srs_trace("stream send to %s finished...\n", url.c_str());

        curl_easy_cleanup(curl);
        curl_slist_free_all(chunk);
    }

    CleanInput();
    srs_trace("thread for stream sender quit..., %s", stream_url_.c_str());
}

void HttpStreamSender::CleanInput()
{
    for (;;) {
        pthread_mutex_lock(&mutex_);
        if (vec_msgs_.empty()) {
            pthread_mutex_unlock(&mutex_);
            break;
        }

        auto it = vec_msgs_.begin();
        TransMsg* msg = *it;
        vec_msgs_.erase(it);
        pthread_mutex_unlock(&mutex_);
        delete msg;
    }
}



HttpStreamReceiver::HttpStreamReceiver(const std::string gate_server, const std::string& stream_url, const StreamRecvCbType& callback) : 
                        StreamReceiver(gate_server, stream_url, callback)
{
    pthread_mutex_init(&mutex_, NULL);
    started_ = false;
    wait_quit_ = false;
    thread_ = NULL;
    multi_timeouts_ = 0;
    multi_handle_ = NULL;
    buf_write_ = NULL;
    data_size_ = 0;
    buf_offset_ = 0;
}

HttpStreamReceiver::~HttpStreamReceiver()
{

}

srs_error_t HttpStreamReceiver::HttpStreamReceiver::Start()
{
    srs_error_t err = srs_success;
    if (started_ || wait_quit_) {
        srs_trace("stream receiver already started, %s\n", stream_url_.c_str());
        return err;
    }

    srs_trace("start stream receiver, %s\n", stream_url_.c_str());
    auto f = [&]() {
        RecvProc();
        wait_quit_ = false;
        started_ = false;
        StopPublish();
    };

    wait_quit_ = false;
    started_ = true;
    if (thread_) {
        delete thread_;
    }
    thread_ = new std::thread(f);
    thread_->detach();
    return err;
}

void HttpStreamReceiver::Stop()
{
    srs_trace("stop stream receiver, %s\n", stream_url_.c_str());
    if (started_) {
        wait_quit_ = true;
        started_ = false;

        if (multi_handle_) {
            curl_multi_wakeup(multi_handle_);
        }

        // close socket
        // https://stackoverflow.com/questions/28767613/cancel-curl-easy-perform-while-it-is-trying-to-connect
        // if (curl_) {
        //     curl_socket_t sockfd;
        //     //发现不起作用，返回的sockfd为-1
        //     auto res = curl_easy_getinfo(curl_, CURLINFO_ACTIVESOCKET, &sockfd);
        //     if(res != CURLE_OK) {
        //         srs_error("Error: %s\n", curl_easy_strerror(res));
        //     } else {
        //         srs_trace("close socket fd(%d) to terminate curl_easy_perform", sockfd);
        //         close(sockfd);
        //     }
        // }
    }
}

static size_t StreamReceiverReadCallback(char *dest, size_t size, size_t nmemb, void *userp)
{
    return ((HttpStreamReceiver*)userp)->RecvMoreCallback(dest, size, nmemb);
}

size_t HttpStreamReceiver::RecvMoreCallback(char *buffer, size_t size, size_t nmemb)
{
    if (first_data_) {
        first_data_ = false;
        srs_trace("interval of send start and read callback:%lld", srs_update_system_time() - tick_start_);
    }

    retry_count_ = 0;
    if (wait_quit_) {
        srs_trace("return for quit recv %s", stream_url_.c_str());
        return CURL_WRITEFUNC_ERROR;
    }

    size_t buffer_size = size * nmemb;
    if (buffer_size <= 0) {
        srs_trace("callback size %d, return for quit recv %s", buffer_size, stream_url_.c_str());
        return CURL_WRITEFUNC_ERROR;
    }

    // uint8_t* data = (uint8_t*)buffer;
    // // 前4个字节总大小
    // uint32_t total_size = ReadUint32BE(data);
    // srs_assert(total_size > 4);
    // srs_trace("RecvMoreCallback, buffer:%p, size:%u, %u", buffer, buffer_size, total_size);

    char* data_read = buffer;
    size_t size_left = buffer_size;

    while (size_left > 0) {

        if (!buf_write_) {
            srs_assert(size_left >= 4);
            uint8_t* data = (uint8_t*)data_read;
            // 前4个字节总大小
            // 注意：服务器的格式要求，total_size不包括前4个字节本身
            uint32_t total_size = ReadUint32BE(data);
            total_size += 4;
            buf_write_ = new char[total_size];
            srs_assert(buf_write_);

            // 写入总数据大小
            WriteUint32BE((uint8_t*)buf_write_, total_size);

            data_size_ = total_size;
            buf_offset_ = 4;

            data_read += 4;
            size_left -= 4;
            continue;
        }

        uint32_t size_need = data_size_ - buf_offset_;

        if (size_need <= size_left) {
            memcpy(buf_write_ + buf_offset_, data_read, size_need);
            size_left -= size_need;
            data_read += size_need;
            // srs_trace("receive packet, size:%u", data_size_);

            if (recv_callback_) {
                TransMsg* msg = new TransMsg;
                srs_assert(msg);
                msg->stream_url = stream_url_;
                msg->type = en_RtcDataType_Media;
                msg->packet = MsgFromRtpExt(stream_url_, (uint8_t*)buf_write_, data_size_);
                recv_callback_(stream_url_, msg);
            }

            delete[] buf_write_;
            buf_write_ = NULL;

        } else {
            memcpy(buf_write_ + buf_offset_, data_read, size_left);
            buf_offset_ += size_left;
            size_left = 0;
        }
    }

    return buffer_size;
}

void HttpStreamReceiver::StopPublish()
{
    if (recv_callback_) {
        srs_trace("stop publish play stream %s", stream_url_.c_str());
        TransMsg* msg = new TransMsg;
        srs_assert(msg);
        msg->stream_url = stream_url_;
        msg->type = en_RtcDataType_UnPublishStreamPlay;
        recv_callback_(stream_url_, msg);
    }
}

void HttpStreamReceiver::RecvProc()
{
    srs_trace("thread for stream receiver start, %s", stream_url_.c_str());

    session_ = (uint64_t)srs_update_system_time();
    tick_start_ = 0;
    retry_count_ = 5;

    for (;;) {

        if (!started_) {
            break;
        }

        if (retry_count_ == 0) {
            break;
        }

        int64_t t0 = srs_update_system_time();
        if (t0 - tick_start_ < 5 * SRS_UTIME_SECONDS) {
            usleep(20000);
            continue;
        }

        tick_start_ = srs_update_system_time();
        first_data_ = true;
        retry_count_--;

        if (!CurlGlobalInit()) {
            break;
        }

#if 0
        CURL *curl;
        CURLcode res;

        curl = curl_easy_init();
        if (!curl) {
            srs_error("curl_easy_init error");
            continue;
        }

        std::string url = "http://" + gate_server_ + qn_get_origin_stream(stream_url_);
        srs_trace("stream receive from %s\n", url.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StreamReceiverReadCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

        struct curl_slist *chunk = NULL;
        std::string session_id = "x-miku-session-id: " + std::to_string(session_);
        // chunk = curl_slist_append(chunk, "Transfer-Encoding: chunked");
        // chunk = curl_slist_append(chunk, "Expect: 100-continue");
        chunk = curl_slist_append(chunk, session_id.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

        res = curl_easy_perform(curl);
        if(res != CURLE_OK) {
            srs_error("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        }

        srs_trace("stream receive from %s finished...\n", url.c_str());

        curl_slist_free_all(chunk);
        curl_easy_cleanup(curl);
#else
        // 使用multi接口
        CURL* curl = curl_easy_init();
        if (!curl) {
            srs_error("curl_easy_init error");
            continue;
        }

        int still_running = 1;
        CURLM* multi_handle;
        multi_handle = curl_multi_init();
        if (!multi_handle) {
            srs_error("curl_multi_init error");
            continue;
        }

        std::string url = "http://" + gate_server_ + qn_get_origin_stream(stream_url_);
        srs_trace("stream receive from %s\n", url.c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StreamReceiverReadCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

        struct curl_slist *chunk = NULL;
        std::string session_id = "x-miku-session-id: " + std::to_string(session_);
        chunk = curl_slist_append(chunk, session_id.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

        curl_multi_add_handle(multi_handle, curl);
        multi_handle_ = multi_handle;

        for (;;) {
            if (!started_) {
                break;
            }

            int numfds;
            CURLMcode mc = curl_multi_perform(multi_handle, &still_running);
            if (mc != CURLM_OK) {
                srs_trace("curl multi perform error, break");
                break;
            }
            
            if (!still_running) {
                srs_trace("curl not running, break");
                break;
            }

            mc = curl_multi_poll(multi_handle, NULL, 0, 1000, &numfds);
            if (mc != CURLM_OK) {
                srs_error("curl multi pool failed, code %d.\n", (int)mc);
                break;
            }

            if (numfds <= 0) {
                multi_timeouts_++;
                if (multi_timeouts_ >= 10) {
                    srs_error("curl multi timeout, break\n");
                    break;
                }

                continue;
            }

            multi_timeouts_ = 0;

            int msgq = 0;
            CURLMsg* m = NULL;
            do {
                m = curl_multi_info_read(multi_handle, &msgq);
            } while (m);
        }

        multi_handle_ = NULL;
        curl_multi_remove_handle(multi_handle, curl);
        curl_easy_cleanup(curl);
        curl_multi_cleanup(multi_handle);
        curl_slist_free_all(chunk);
        srs_trace("stream receive from %s finished...\n", url.c_str());
#endif
    }

    srs_trace("thread for stream receiver quit..., %s", stream_url_.c_str());
}
