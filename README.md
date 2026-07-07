# High-Frequency-Trading-Engine (C++)

An end-to-end **HFT-style trading system simulator** in modern C++: market-data ingest → book building → strategy → pre-trade risk → execution → fills → PnL/positions.  
Goal: build a correct system first, then make it low-latency with measurements.

---

## Architecture (big picture)

```mermaid
flowchart LR
  %% Theme hints (rendered by GitHub-compatible Mermaid)
  %%{init: {'flowchart': {'curve': 'basis'}, 'theme': 'dark'} }%%

  EX[Exchange Simulator\n(venue)] -->|market data| MD[Market Data\nFeed Handler + Decoder]
  MD --> BB[Order Book Builder\n(reconstruct book)]
  BB --> ST[Strategy Engine\n(signals → intents)]
  ST --> RK[Risk Engine\n(pre-trade checks)]
  RK --> GW[Order Gateway / OMS\n(send orders)]
  GW -->|orders| EX
  EX -->|fills / acks| GW
  GW --> FP[Fills Processor\n(update positions/PnL)]
  FP --> RK
  FP --> ST

  classDef core fill:#1f2937,stroke:#93c5fd,stroke-width:1px,color:#e5e7eb;
  classDef hot fill:#111827,stroke:#f59e0b,stroke-width:1px,color:#f9fafb;
  classDef io fill:#0b1220,stroke:#34d399,stroke-width:1px,color:#ecfeff;

  class MD,BB,ST,RK hot;
  class EX,GW,FP io;
```

---

## What’s implemented (current repo state)

```mermaid
flowchart TB
  %%{init: {'theme': 'dark'} }%%

  subgraph common["src/common"]
    T["types.h\n(OrderRequest, Side, Fill, Order)"]
    MP["memory_pool.h\n(MemoryPool<T>)"]
  end

  subgraph marketdata["src/marketdata"]
    FH["feed_handler\n(UDP packet parsing + dispatch)"]
    PD["decoders/pitch_decoder\n(zero-copy-ish decode via memcpy)"]
    OBB["book_builder/order_book_builder\n(level aggregation + snapshot)"]
  end

  subgraph core["src/core"]
    LOB["orderbook/limit_order_book\n(map levels + FIFO queues)"]
    ME["matching_engine/\n(TODO)"]
  end

  subgraph strategy["src/strategy"]
    BA["book_adapter.h\n(builder → BookUpdate)"]
    SE["strategy_engine\n(momentum/imbalance/VWAP dev/spread)"]
  end

  subgraph risk["src/risk"]
    RE["risk_engine\n(chain checks: position/notional/rate/CB/dedupe)"]
  end

  subgraph exec["src/execution"]
    OM["order_manager\n(order lifecycle)"]
    FG["order_gateway\n(TODO / partial)"]
    FP["fills_processor\n(consume fills → update state)"]
  end

  subgraph net["src/network"]
    N["(TODO)"]
  end

  FH --> PD --> OBB
  OBB --> BA --> SE --> RE --> OM --> FG
  FG --> FP
  T --- MP

  classDef done fill:#052e2b,stroke:#34d399,stroke-width:1px,color:#ecfeff;
  classDef todo fill:#2b1d0e,stroke:#f59e0b,stroke-width:1px,color:#fff7ed;
  classDef mid fill:#111827,stroke:#93c5fd,stroke-width:1px,color:#e5e7eb;

  class T,MP,FH,PD,OBB,LOB,BA,SE,RE,OM,FP done;
  class ME,FG,N todo;
```

---

## Data flow (tick → trade)

```mermaid
sequenceDiagram
  %%{init: {'theme': 'dark'} }%%
  participant EX as ExchangeSim
  participant FH as FeedHandler
  participant BB as OrderBookBuilder
  participant ST as StrategyEngine
  participant RK as RiskEngine
  participant GW as OrderGateway/OMS
  participant FP as FillsProcessor

  EX->>FH: packet(bytes)
  FH->>BB: FeedMessage(Add/Cancel/Trade)
  BB->>ST: BookUpdate(best bid/ask)
  ST->>RK: OrderRequest(intent)
  RK-->>ST: PASS/FAIL + reason
  RK->>GW: PASS → send intent
  GW->>EX: order(s)
  EX-->>GW: fill/ack
  GW->>FP: ExecFill
  FP->>RK: onFill(position/PnL update)
  FP->>ST: fill event (optional)
```

---

## Build & run (today)

This repo currently emphasizes **component correctness** with standalone unit tests (no external framework).

Example (limit order book unit tests):

```bash
g++ -std=c++17 tests/unit/test_limit_order_book.cpp \
  src/core/orderbook/limit_order_book.cpp \
  -o test_lob && ./test_lob
```

> Roadmap includes adding a full build system (CMake), CI, sanitizers, benchmarks, and end-to-end apps. See `PROJECT_PLAN.txt`.

---

## Project roadmap (high level)

- **Correctness first**: limit order book + matching engine + replayable tests
- **End-to-end loop**: exchange simulator + gateway + fills + positions/PnL
- **Low-latency engineering**: lock-free queues, cache-friendly book, no hot-path allocations
- **Measurement**: tick-to-trade latency histograms (p50/p99/p99.9) and microbenchmarks

For the detailed plan, see `PROJECT_PLAN.txt`.

