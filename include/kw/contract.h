/* The contract: the send hierarchy the system is supposed to have.
 *
 *   # servers low, clients high: a process may only send to a lower layer
 *   layer 0 procnto*
 *   layer 1 devc-* io-sock
 *   layer 2 wheel_sensor
 *   layer 3 brake_ctl
 *   ignore  hmi_* logger     # known pulse-only link, exclude from analysis
 *   strict                   # every process must be assigned a layer
 *
 * Process names are matched with shell globs; the first matching `layer`
 * line wins. */
#ifndef KW_CONTRACT_H
#define KW_CONTRACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    int layer;
    char *glob;
} kw_layer_rule;

typedef struct {
    char *client_glob;
    char *server_glob;
} kw_ignore_rule;

typedef struct {
    kw_layer_rule *layers;
    size_t n_layers, cap_layers;
    kw_ignore_rule *ignores;
    size_t n_ignores, cap_ignores;
    bool strict;
} kw_contract;

void kw_contract_init(kw_contract *c);
void kw_contract_free(kw_contract *c);

/* Parse a contract. Returns 0, or -1 with a message naming the line. */
int kw_contract_read(FILE *in, kw_contract *c, char *err, size_t err_size);

/* Layer assigned to `name`, or -1 if no rule matches. */
int kw_contract_layer(const kw_contract *c, const char *name);

/* True if the edge client -> server is excluded by an `ignore` rule. `c` may
 * be NULL (nothing is ignored). */
bool kw_contract_ignored(const kw_contract *c, const char *client, const char *server);

#endif
