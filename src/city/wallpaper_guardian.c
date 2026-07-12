#include "wallpaper_guardian.h"

#include "city/constants.h"
#include "city/finance.h"
#include "city/gods.h"
#include "city/health.h"
#include "city/sentiment.h"

#define GUARDIAN_MAX_GOD_MOOD 100
#define GUARDIAN_MAX_HAPPINESS 100
#define GUARDIAN_MAX_HEALTH 100
#define GUARDIAN_TREASURY_FLOOR 1000

void wallpaper_guardian_update(void)
{
    city_sentiment_set_happiness(GUARDIAN_MAX_HAPPINESS);
    city_sentiment_reset_protesters_criminals();
    city_health_set(GUARDIAN_MAX_HEALTH);
    city_god_set_happiness(GOD_ALL, GUARDIAN_MAX_GOD_MOOD);

    int treasury = city_finance_treasury();
    if (treasury < GUARDIAN_TREASURY_FLOOR) {
        city_finance_treasury_add(GUARDIAN_TREASURY_FLOOR - treasury);
    }
}
