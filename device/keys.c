/* Factory keys "in the firmware image" (DESIGN §10.1, §15.3 note). For the demo they are derived
 * deterministically so conf/keys.json can hold the same values:
 *   K_fleet        = SHA-256("ZTP-DEMO-K_fleet")
 *   K_mfg[model]   = SHA-256("ZTP-DEMO-K_mfg:" || model)
 * A real product embeds random per-fleet and per-model secrets instead. A unit test checks that
 * conf/keys.json matches this derivation. */
#include <string.h>

#include "dev.h"

void dev_factory_keys(const char *model, uint8_t k_fleet[ZTP_KEY_LEN], uint8_t k_mfg[ZTP_KEY_LEN])
{
    static const char fleet_label[] = "ZTP-DEMO-K_fleet", mfg_label[] = "ZTP-DEMO-K_mfg:";
    sha256(fleet_label, sizeof fleet_label - 1, k_fleet);
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, mfg_label, sizeof mfg_label - 1);
    sha256_update(&c, model, strlen(model));
    sha256_final(&c, k_mfg);
}
