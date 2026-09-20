#include "zt_game.h"
#include <stdbool.h>
#include <string.h>
bool zt_game_is_owner(void);
zt_err_t zt_game_queue_invalidation(uint32_t generation);

typedef struct {
    zt_peer_entry_t value;
    uint64_t samples[ZT_RSSI_MIN_SAMPLES];
    zt_boot_nonce_t boot;
    uint32_t packet_seq;
    uint8_t used, next;
} peer_t;
static peer_t peers[ZT_MAX_PLAYERS+ZT_MAX_LOBBY_DISCOVERIES];
static const zt_checkpoint_t *roster;
static zt_game_id_t game;
static uint32_t generation;
static int8_t threshold=ZT_TAG_RSSI_DEFAULT_DBM;
/* Private interfaces, called only by the game owner. Roster storage belongs to
 * that owner and stays at the same address for the admitted round. */
void zt_game_peers_configure(zt_game_id_t id, const zt_checkpoint_t *cp);
void zt_game_peers_configure(zt_game_id_t id, const zt_checkpoint_t *cp)
{
    game=id; roster=cp; threshold=cp ? cp->rules.tag_rssi : ZT_TAG_RSSI_DEFAULT_DBM;
    memset(peers,0,sizeof(peers));
}
zt_err_t zt_peers_invalidate(uint32_t driver_generation)
{
    if (!zt_game_is_owner()) return zt_game_queue_invalidation(driver_generation);
    generation=driver_generation; memset(peers,0,sizeof(peers)); return ZT_OK;
}
zt_err_t zt_peers_observe(const zt_domain_message_t *m)
{
    if (!m) return ZT_ERR_INVALID_ARG;
    if (!zt_game_is_owner()) return zt_game_post_domain(m);
    if (m->header.type!=ZT_PKT_BEACON || m->header.game_id!=game || m->header.hops || m->header.ttl_remaining ||
        m->driver_generation!=generation || memcmp(m->header.origin.bytes,m->direct_source.bytes,6)) return ZT_ERR_PROTOCOL;
    const zt_wire_beacon_t *b=&m->payload.beacon;
    int at=-1;
    if (roster) {
        if (m->header.round_id!=roster->round_id || m->channel!=roster->round_channel) return ZT_ERR_STALE;
        for (unsigned i=0;i<roster->roster_count;++i) if (roster->roster[i].slot==b->slot && !memcmp(roster->roster[i].mac.bytes,m->direct_source.bytes,6)) { at=i; break; }
        if (at<0) return ZT_ERR_AUTH;
    } else {
        for (unsigned i=20;i<28;++i) {
            if (peers[i].used && !memcmp(peers[i].value.mac.bytes,m->direct_source.bytes,6)) { at=i; break; }
            if (at<0 && (!peers[i].used || (m->rx_us>=peers[i].samples[(peers[i].next+2)%3] &&
                m->rx_us-peers[i].samples[(peers[i].next+2)%3]>=ZT_PEER_EVICT_MS*1000ULL))) at=i;
        }
        if (at<0) return ZT_ERR_NO_SPACE;
    }
    peer_t *p=&peers[at];
    if (p->used && memcmp(p->value.mac.bytes,m->direct_source.bytes,6)) memset(p,0,sizeof(*p));
    if (p->used && p->boot==m->header.origin_boot_nonce && m->header.packet_seq<=p->packet_seq) return ZT_ERR_STALE;
    uint64_t latest=p->samples[(p->next+2)%3];
    if (p->used && m->rx_us<latest) return ZT_ERR_STALE;
    if (p->used && (p->boot!=m->header.origin_boot_nonce || m->rx_us-latest>=ZT_PEER_EVICT_MS*1000ULL)) memset(p,0,sizeof(*p));
    p->value.filtered_rssi_q8=p->used ? (3*(int32_t)p->value.filtered_rssi_q8+2*(int32_t)m->rssi*256)/5 : (int16_t)m->rssi*256;
    p->used=1; p->boot=m->header.origin_boot_nonce; p->packet_seq=m->header.packet_seq;
    p->samples[p->next]=m->rx_us; p->next=(p->next+1)%3;
    p->value.mac=m->direct_source; p->value.slot=b->slot; p->value.role=b->role; p->value.role_rev=b->role_rev;
    p->value.cause=(zt_cause_t){b->role==ZT_ROLE_ZOMBIE ? b->slot : ZT_SLOT_INVALID,b->infection_cause_seq};
    p->value.latest_rssi=m->rssi; p->value.provisional=!!(b->flags&ZT_BEACON_FLAG_PROVISIONAL);
    if (roster) { memcpy(p->value.name,roster->roster[at].name,12); p->value.name[12]=0; }
    return ZT_OK;
}
zt_err_t zt_peers_list(uint64_t now_us, zt_peer_entry_t *out, size_t cap, size_t *count)
{
    if (!out || !count) return ZT_ERR_INVALID_ARG;
    if (!zt_game_is_owner()) return ZT_ERR_INVALID_STATE;
    *count=0;
    for (unsigned i=0;i<28;++i) {
        peer_t *p=&peers[i]; if (!p->used) continue;
        uint64_t latest=p->samples[(p->next+2)%3]; if (now_us<latest) continue;
        uint64_t age=(now_us-latest)/1000;
        if (age>=ZT_PEER_EVICT_MS) { memset(p,0,sizeof(*p)); continue; }
        if (age>=ZT_PEER_STALE_MS) continue;
        if (*count==cap) return ZT_ERR_NO_SPACE;
        zt_peer_entry_t v=p->value; v.age_ms=age; v.recent_sample_count=0;
        for (unsigned j=0;j<3;++j) if (p->samples[j] && now_us>=p->samples[j] && now_us-p->samples[j]<=ZT_RSSI_SAMPLE_WINDOW_MS*1000ULL) ++v.recent_sample_count;
        v.eligible=roster && v.recent_sample_count>=3 && age<=ZT_TAG_SOURCE_MAX_AGE_MS &&
            v.filtered_rssi_q8>=threshold*256 && v.latest_rssi>=threshold;
        int16_t r=v.filtered_rssi_q8;
        v.tier=r>=ZT_TIER_IN_RANGE_DBM*256 ? ZT_RANGE_IN_RANGE : r>=ZT_TIER_CLOSE_DBM*256 ? ZT_RANGE_CLOSE : r>=ZT_TIER_NEARBY_DBM*256 ? ZT_RANGE_NEARBY : ZT_RANGE_FAR;
        size_t pos=*count;
        while (pos && (out[pos-1].filtered_rssi_q8<v.filtered_rssi_q8 ||
            (out[pos-1].filtered_rssi_q8==v.filtered_rssi_q8 && out[pos-1].slot>v.slot))) { out[pos]=out[pos-1]; --pos; }
        out[pos]=v; ++*count;
    }
    return ZT_OK;
}
