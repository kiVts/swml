#pragma once
#include "swml_v4.h"
#include "checked_math.h"
#include <array>
#include <cstring>
#include <map>
#include <vector>

namespace swml4::messages {
enum class Kind:uint8_t { request=1,reply=2,snapshot=3,error=4 };
inline constexpr size_t header_size=56,max_payload=SWML_NET_MAX_PAYLOAD-header_size;
struct Packet {
    Kind kind=Kind::request;
    uint32_t schema=1,operation=1;
    uint64_t request=0,session_epoch=0,revision=0;
    std::array<uint8_t,16> world{};
    std::vector<uint8_t> payload;
};
inline bool same_peer(const swml_peer& a,const swml_peer& b){return a.generation==b.generation&&memcmp(a.identity,b.identity,16)==0;}
inline void append(std::vector<uint8_t>& bytes,uint64_t value,unsigned count){for(unsigned i=0;i<count;++i)bytes.push_back(uint8_t(value>>(8*i)));}
inline uint64_t integer(const uint8_t* bytes,unsigned count){uint64_t value=0;for(unsigned i=0;i<count;++i)value|=uint64_t(bytes[i])<<(8*i);return value;}
inline swml_result encode(const Packet& packet,std::vector<uint8_t>& bytes){
    if(packet.payload.size()>max_payload||!packet.schema||!packet.operation||uint8_t(packet.kind)<1||uint8_t(packet.kind)>4||
       (packet.kind==Kind::snapshot?packet.request!=0:packet.request==0))return SWML_INVALID;
    bytes={'S','4','M',1,uint8_t(packet.kind),0,0,0};append(bytes,packet.schema,4);append(bytes,packet.operation,4);append(bytes,packet.request,8);
    bytes.insert(bytes.end(),packet.world.begin(),packet.world.end());append(bytes,packet.session_epoch,8);append(bytes,packet.revision,8);
    bytes.insert(bytes.end(),packet.payload.begin(),packet.payload.end());return SWML_OK;
}
inline swml_result decode(const void* input,size_t size,Packet& out){
    if(!input||size<header_size||size>SWML_NET_MAX_PAYLOAD)return SWML_INVALID;auto* bytes=static_cast<const uint8_t*>(input);
    if(memcmp(bytes,"S4M\x01",4)||bytes[4]<1||bytes[4]>4||bytes[5]||bytes[6]||bytes[7])return SWML_INVALID;
    Packet packet;packet.kind=Kind(bytes[4]);packet.schema=uint32_t(integer(bytes+8,4));packet.operation=uint32_t(integer(bytes+12,4));packet.request=integer(bytes+16,8);
    memcpy(packet.world.data(),bytes+24,16);packet.session_epoch=integer(bytes+40,8);packet.revision=integer(bytes+48,8);
    if(!packet.schema||!packet.operation||(packet.kind==Kind::snapshot?packet.request!=0:packet.request==0))return SWML_INVALID;
    packet.payload.assign(bytes+header_size,bytes+size);out=std::move(packet);return SWML_OK;
}
// No background callbacks/threads: drive this bounded correlation table on the
// game lane. The peer comes from swml_network_message, never from payload data.
class PendingReplies {
    struct Pending {swml_peer peer;Packet request;uint64_t deadline;};
    std::map<uint64_t,Pending> pending;
    uint64_t next=1;
public:
    swml_result begin(const swml_peer& peer,const std::array<uint8_t,16>& world,uint64_t session,uint32_t schema,uint32_t operation,
                       const void* payload,uint32_t size,uint64_t now,uint64_t timeout,Packet& request){
        if(size>max_payload||(size&&!payload)||!schema||!operation||!timeout||timeout>60000||!peer.generation)return SWML_INVALID;
        uint64_t deadline=0;if(!swml_math::add(now,timeout,deadline))return SWML_INVALID;
        if(pending.size()>=128||!next)return SWML_LIMIT;
        Packet packet;packet.kind=Kind::request;packet.request=next++;packet.world=world;packet.session_epoch=session;packet.schema=schema;packet.operation=operation;
        if(size)packet.payload.assign(static_cast<const uint8_t*>(payload),static_cast<const uint8_t*>(payload)+size);
        pending.emplace(packet.request,Pending{peer,packet,deadline});request=std::move(packet);return SWML_OK;
    }
    swml_result complete(const swml_peer& source,const Packet& reply,uint64_t now,std::vector<uint8_t>& payload,bool& remote_error){
        if(reply.kind!=Kind::reply&&reply.kind!=Kind::error)return SWML_INVALID;
        auto it=pending.find(reply.request);if(it==pending.end())return SWML_NOT_FOUND;
        if(now>=it->second.deadline){pending.erase(it);return SWML_NOT_FOUND;}
        const auto& request=it->second.request;
        if(!same_peer(source,it->second.peer)||reply.world!=request.world||reply.session_epoch!=request.session_epoch||reply.schema!=request.schema||reply.operation!=request.operation)return SWML_DENIED;
        if(reply.payload.size()>max_payload)return SWML_INVALID;
        payload=reply.payload;remote_error=reply.kind==Kind::error;pending.erase(it);return SWML_OK;
    }
    std::vector<uint64_t> expire(uint64_t now){
        std::vector<uint64_t> expired;for(auto it=pending.begin();it!=pending.end();)if(now>=it->second.deadline){expired.push_back(it->first);it=pending.erase(it);}else++it;return expired;
    }
    bool cancel(uint64_t id){return pending.erase(id)!=0;}
    void clear(){pending.clear();}
    size_t size()const{return pending.size();}
};
class SnapshotGate {
    swml_peer peer{};std::array<uint8_t,16> world{};uint64_t session=0,revision=0;uint32_t schema=0,operation=0;bool ready=false;
public:
    void configure(const swml_peer& verified_peer,std::array<uint8_t,16> expected_world,uint64_t negotiated_session,uint32_t expected_schema,uint32_t expected_operation){
        peer=verified_peer;world=expected_world;session=negotiated_session;schema=expected_schema;operation=expected_operation;revision=0;
        ready=peer.generation!=0&&session!=0&&schema!=0&&operation!=0;
    }
    bool accept(const swml_peer& source,const Packet& packet){
        if(!ready||packet.kind!=Kind::snapshot||packet.request||!same_peer(source,peer)||packet.world!=world||packet.session_epoch!=session||
            packet.schema!=schema||packet.operation!=operation||packet.revision<=revision||packet.payload.size()>max_payload)return false;
        revision=packet.revision;return true;
    }
    void reset(){ready=false;revision=0;}
};
} // namespace swml4::messages
