#include "zt_game.h"
#include <stdbool.h>
#include <limits.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
bool zt_game_is_owner(void);
zt_err_t zt_game_queue_clock(zt_round_id_t round,const zt_clock_sample_t *sample);
static portMUX_TYPE clock_guard=portMUX_INITIALIZER_UNLOCKED;

static zt_round_id_t clock_round;
static zt_clock_sample_t anchor;
static int64_t deadline_us;
static bool valid;

/* Private game-owner reset, used only for a different admitted round. No saved
 * checkpoint is ever an elapsed-time anchor. */
void zt_game_clock_reset(void);
void zt_game_clock_reset(void)
{
    portENTER_CRITICAL(&clock_guard); valid=false; clock_round=0; deadline_us=0; portEXIT_CRITICAL(&clock_guard);
}
zt_err_t zt_clock_apply(zt_round_id_t round_id, const zt_clock_sample_t *sample)
{
    if (!sample || !round_id) return ZT_ERR_INVALID_ARG;
    /* Gateway clock exchanges copy into the game queue. Only its owner can
     * update the anchor; the synchronous owner path is used by radio samples. */
    if (!zt_game_is_owner()) return zt_game_queue_clock(round_id,sample);
    uint64_t now=(uint64_t)esp_timer_get_time();
    if (sample->quality!=ZT_TIME_INITIALIZED || sample->elapsed_ms==INT32_MIN ||
        sample->uncertainty_ms>ZT_TIME_UNCERTAINTY_MAX_MS || sample->sampled_us>now) return ZT_ERR_STALE;
    uint64_t age=now-sample->sampled_us;
    uint64_t drift=(age*ZT_CLOCK_DRIFT_PPM+999999999ULL)/1000000000ULL;
    if (drift+sample->uncertainty_ms>ZT_TIME_UNCERTAINTY_MAX_MS) return ZT_ERR_STALE;
    int64_t proposed=(int64_t)sample->sampled_us+((int64_t)ZT_ROUND_DURATION_MS-sample->elapsed_ms)*1000;
    if (valid && round_id!=clock_round) return ZT_ERR_CONFLICT;
    int64_t accepted_deadline=valid && deadline_us<proposed ? deadline_us : proposed;
    int64_t elapsed=(int64_t)ZT_ROUND_DURATION_MS-((accepted_deadline-(int64_t)now)/1000);
    if (elapsed>INT32_MAX || elapsed<=INT32_MIN) return ZT_ERR_OVERFLOW;
    portENTER_CRITICAL(&clock_guard);
    /* Commit only after validation; even a refused sample leaves the learned
     * deadline intact, and an accepted sample can only shorten it. */
    deadline_us=accepted_deadline;
    anchor=(zt_clock_sample_t){(int32_t)elapsed,sample->uncertainty_ms+(uint32_t)drift,now,ZT_TIME_INITIALIZED};
    clock_round=round_id; valid=true;
    portEXIT_CRITICAL(&clock_guard);
    return ZT_OK;
}
zt_err_t zt_clock_read(uint64_t now_us, zt_clock_sample_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    *out=(zt_clock_sample_t){INT32_MIN,UINT32_MAX,now_us,ZT_TIME_UNKNOWN};
    portENTER_CRITICAL(&clock_guard); zt_clock_sample_t copy=anchor; bool initialized=valid; portEXIT_CRITICAL(&clock_guard);
    if (!initialized || now_us<copy.sampled_us) return ZT_ERR_INVALID_STATE;
    uint64_t delta=now_us-copy.sampled_us;
    int64_t elapsed=(int64_t)copy.elapsed_ms+(int64_t)(delta/1000);
    uint64_t uncertainty=copy.uncertainty_ms+(delta*ZT_CLOCK_DRIFT_PPM+999999999ULL)/1000000000ULL;
    out->elapsed_ms=elapsed>INT32_MAX ? INT32_MAX : (int32_t)elapsed;
    out->uncertainty_ms=uncertainty>UINT32_MAX ? UINT32_MAX : (uint32_t)uncertainty;
    out->quality=uncertainty<=ZT_TIME_UNCERTAINTY_MAX_MS ? ZT_TIME_INITIALIZED : ZT_TIME_UNKNOWN;
    return ZT_OK;
}
