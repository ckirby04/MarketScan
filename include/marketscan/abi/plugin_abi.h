/*
 * MarketScan -- the plugin boundary.
 *
 * PURE C. This file is the contract between the host and every theory plugin,
 * and it is the only thing they are allowed to say to each other.
 *
 * Rules, all of which exist because of a specific way this goes wrong:
 *
 *   - No C++ types cross this boundary. On MSVC the debug and release CRTs use
 *     different std::string and std::vector layouts, so a plugin built in the
 *     wrong configuration silently corrupts memory rather than failing to
 *     load. Every type here is a POD of fixed-width scalars.
 *
 *   - No exception may unwind across this boundary. The vtable shims generated
 *     by MARKETSCAN_EXPORT_STRATEGY catch everything.
 *
 *   - No allocation crosses this boundary. Whoever allocates, frees.
 *
 *   - const char* returned by the host stays valid until on_stop returns.
 *     const char* handed to the host is only valid for that call.
 *
 *   - A plugin takes time, randomness, order flow and account state from
 *     MS_HostContext and from nowhere else. A strategy that reads the wall
 *     clock or calls rand() is not replayable, and an unreproducible backtest
 *     is not evidence.
 *
 * This ABI is FROZEN. Additive changes require a MS_ABI_VERSION bump and a
 * loader that can refuse the old version cleanly.
 */
#ifndef MARKETSCAN_PLUGIN_ABI_H
#define MARKETSCAN_PLUGIN_ABI_H

#include <stdint.h>

#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped only for an incompatible change. The loader refuses any mismatch. */
#define MS_ABI_VERSION 1u

typedef int64_t MS_Nanos; /* nanoseconds since the Unix epoch, UTC */
typedef int64_t MS_Money; /* micro-dollars: 1 USD == 1000000 */
typedef uint32_t MS_SymbolId;

typedef enum { MS_SIDE_BUY = 0, MS_SIDE_SELL = 1 } MS_Side;

typedef enum { MS_ORD_MARKET = 0, MS_ORD_LIMIT = 1 } MS_OrderType;

typedef enum { MS_TIF_DAY = 0, MS_TIF_IOC = 1, MS_TIF_GTC = 2 } MS_TimeInForce;

typedef enum { MS_LOG_DEBUG = 0, MS_LOG_INFO = 1, MS_LOG_WARN = 2, MS_LOG_ERROR = 3 } MS_LogLevel;

typedef struct {
    MS_Nanos ts;
    MS_SymbolId symbol;
    double open, high, low, close, volume;
} MS_Bar;

typedef struct {
    MS_Nanos ts;
    MS_SymbolId symbol;
    double bid_px, ask_px, bid_sz, ask_sz;
} MS_Quote;

typedef struct {
    MS_Nanos ts;
    MS_SymbolId symbol;
    double price, size;
    MS_Side aggressor;
} MS_Trade;

typedef struct {
    MS_Nanos ts;
    MS_SymbolId symbol;
    uint64_t order_id;
    /* The strategy's own id from the originating MS_OrderRequest, echoed back
       so a strategy can match a fill to its intent without keeping a map from
       engine order ids. Added before the ABI froze; MS_OrderRequest always
       promised this would come back and there was nowhere for it to arrive. */
    uint64_t client_order_id;
    MS_Side side;
    double price, quantity;
    MS_Money commission;
} MS_Fill;

typedef struct {
    MS_SymbolId symbol;
    MS_Side side;
    MS_OrderType type;
    MS_TimeInForce tif;
    double quantity;
    double limit_price;       /* ignored when type == MS_ORD_MARKET */
    uint64_t client_order_id; /* strategy-assigned, echoed back on fill */
} MS_OrderRequest;

/*
 * Services the host provides to the plugin. The plugin must use these and
 * nothing else for time, randomness, order flow and account state.
 *
 * Every function takes the opaque `host` handle as its first argument. All of
 * them are safe to call from any strategy callback and from nowhere else.
 */
typedef struct MS_HostContext {
    void* host; /* opaque host handle, pass back as first arg */

    /* Returns the engine-assigned order id, or 0 if the order was rejected. */
    uint64_t (*submit_order)(void* host, const MS_OrderRequest* req);
    void (*cancel_order)(void* host, uint64_t order_id);

    /* The only clock a strategy may read. Replay and live differ only here. */
    MS_Nanos (*now)(void* host);
    /* Uniform on [0, 1). Seeded per run, so replays reproduce exactly. */
    double (*random_uniform)(void* host);

    double (*position)(void* host, MS_SymbolId s); /* signed units */
    MS_Money (*cash)(void* host);
    MS_Money (*equity)(void* host);

    /* Returns 0 for an unknown ticker. Ids are stable for the run. */
    MS_SymbolId (*symbol_id)(void* host, const char* ticker);
    /* Valid until on_stop returns. Never NULL for a valid id. */
    const char* (*symbol_name)(void* host, MS_SymbolId s);
    uint32_t (*symbol_count)(void* host);

    void (*log)(void* host, MS_LogLevel lvl, const char* msg);
    /* Publish a named scalar; the GUI plots it automatically. */
    void (*publish_metric)(void* host, const char* name, double value);
} MS_HostContext;

typedef enum {
    MS_PARAM_DOUBLE = 0,
    MS_PARAM_INT = 1,
    MS_PARAM_BOOL = 2,
    MS_PARAM_ENUM = 3
} MS_ParamType;

/* Describes one tunable. The GUI renders a control from this automatically. */
typedef struct {
    const char* name;
    const char* description;
    MS_ParamType type;
    double min_value, max_value, default_value;
    const char* const* enum_labels; /* NULL unless MS_PARAM_ENUM */
    uint32_t enum_count;
} MS_ParamDesc;

/*
 * Static description of a strategy. Returned by ms_plugin_describe before any
 * instance exists, so everything in it must have static storage duration.
 */
typedef struct {
    uint32_t abi_version; /* must equal MS_ABI_VERSION */
    const char* id;       /* stable unique id, e.g. "sma_crossover" */
    const char* display_name;
    const char* author;
    const char* description;
    const MS_ParamDesc* params;
    uint32_t param_count;
    const char* const* required_symbols; /* NULL == subscribe to all */
    uint32_t required_symbol_count;
} MS_StrategyDesc;

/*
 * Instance vtable. Any callback may be NULL if the strategy does not need it;
 * the host checks before calling. `self` is the pointer ms_plugin_create
 * returned.
 */
typedef struct {
    void (*destroy)(void* self);
    void (*on_start)(void* self, const MS_HostContext* host);
    void (*on_bar)(void* self, const MS_Bar* bar);
    void (*on_quote)(void* self, const MS_Quote* quote);
    void (*on_trade)(void* self, const MS_Trade* trade);
    void (*on_fill)(void* self, const MS_Fill* fill);
    void (*on_stop)(void* self);
    void (*set_param)(void* self, const char* name, double value);
    double (*get_param)(void* self, const char* name);
} MS_StrategyVTable;

/* --- The three symbols every plugin DLL must export --- */

/* Called first, and alone, before anything else in the DLL is touched. */
MS_EXPORT uint32_t ms_plugin_abi_version(void);

MS_EXPORT const MS_StrategyDesc* ms_plugin_describe(void);

/* Fills out_vtable and returns the instance, or NULL on failure. */
MS_EXPORT void* ms_plugin_create(MS_StrategyVTable* out_vtable);

/* Matching function pointer types, for the loader's GetProcAddress casts. */
typedef uint32_t (*MS_PluginAbiVersionFn)(void);
typedef const MS_StrategyDesc* (*MS_PluginDescribeFn)(void);
typedef void* (*MS_PluginCreateFn)(MS_StrategyVTable*);

#ifdef __cplusplus
} /* extern "C" */

/*
 * Layout guards. These are not style checks: if one of them ever fires, a
 * plugin compiled against an older copy of this header is about to read the
 * wrong bytes. Changing a size here means bumping MS_ABI_VERSION.
 */
static_assert(sizeof(MS_Bar) == 56, "MS_Bar layout changed; bump MS_ABI_VERSION");
static_assert(sizeof(MS_Quote) == 48, "MS_Quote layout changed; bump MS_ABI_VERSION");
static_assert(sizeof(MS_Trade) == 40, "MS_Trade layout changed; bump MS_ABI_VERSION");
static_assert(sizeof(MS_Fill) == 64, "MS_Fill layout changed; bump MS_ABI_VERSION");
static_assert(sizeof(MS_OrderRequest) == 40, "MS_OrderRequest layout changed; bump MS_ABI_VERSION");
static_assert(sizeof(MS_Nanos) == 8, "MS_Nanos must be 64-bit");
static_assert(sizeof(MS_Money) == 8, "MS_Money must be 64-bit");

#endif /* __cplusplus */

#endif /* MARKETSCAN_PLUGIN_ABI_H */
