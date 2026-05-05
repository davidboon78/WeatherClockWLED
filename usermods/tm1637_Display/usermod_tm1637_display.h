#pragma once
#ifdef USERMOD_TM1637_DISPLAY

/*
 * TM1637 Display Usermod — public API header
 *
 * The full implementation lives in usermod_tm1637_display.cpp.
 * This file is intentionally minimal so the WLED usermod loader's generated
 * #include does not create a duplicate class definition.
 *
 * Display cycle
 * -------------
 * The display rotates through a queue of DisplaySlots.  Each cycle starts with
 * the clock face (shown for `time-duration-ms`), then plays through every slot
 * in the queue (each for its own `durationMs`).  When the queue empties it is
 * rebuilt from scratch: weather slots are added automatically, then every
 * registered SlotProvider callback is invoked so external usermods can inject
 * their own slots (e.g. a live baseball score).
 *
 * Slot types
 * ----------
 * SLOT_TIME      — clock face, never queued (fills inter-cycle gap)
 * SLOT_TEMP      — temperature in °C
 * SLOT_CONDITION — weather condition abbreviation + severity
 * SLOT_BASEBALL  — live baseball score (4-char text, injected by TM1637_Clock)
 * SLOT_CUSTOM    — arbitrary 4-char text via tm1637DisplayShowMessage()
 *
 * Adding a new data source
 * ------------------------
 * 1. Call tm1637DisplayAddSlotProvider(myFn) from your usermod's setup().
 * 2. In myFn, call queue.push(SLOT_BASEBALL / SLOT_CUSTOM, durationMs, text)
 *    to append slots.  The function is called once per cycle rebuild.
 *
 * Connections (NodeMCU v3):
 *   TM1637 CLK -> D5 (GPIO14)
 *   TM1637 DIO -> D6 (GPIO12)
 *   TM1637 VCC -> 3.3V
 *   TM1637 GND -> GND
 */

// ---------------------------------------------------------------------------
// TM1637SlotType — identifies what a display slot renders
// SLOT_TIME is never queued; it fills the inter-cycle clock gap automatically.
// ---------------------------------------------------------------------------
enum TM1637SlotType : uint8_t {
  SLOT_TIME       = 0,  // clock face (not queued)
  SLOT_TEMP       = 1,  // outside temperature in °C
  SLOT_CONDITION  = 2,  // weather condition abbreviation + severity
  SLOT_BASEBALL   = 3,  // live baseball score (4-char text, e.g. "3-2")
  SLOT_CUSTOM     = 4,  // arbitrary 4-char text via tm1637DisplayShowMessage()
};

// ---------------------------------------------------------------------------
// DisplaySlot — one entry in the display queue
// ---------------------------------------------------------------------------
struct DisplaySlot {
  TM1637SlotType type;
  uint16_t       durationMs;
  char           text[5];  // used by SLOT_BASEBALL and SLOT_CUSTOM; null-terminated
};

// ---------------------------------------------------------------------------
// SlotQueue — simple fixed-capacity FIFO
// Defined here so SlotProviderFn callbacks can call queue.push().
// ---------------------------------------------------------------------------
#define TM1637D_QUEUE_MAX 8

struct SlotQueue {
  DisplaySlot slots[TM1637D_QUEUE_MAX];
  uint8_t     head  = 0;
  uint8_t     count = 0;

  bool empty() const { return count == 0; }

  bool push(TM1637SlotType type, uint16_t durationMs, const char* text = nullptr) {
    if (count >= TM1637D_QUEUE_MAX) return false;
    uint8_t tail = (head + count) % TM1637D_QUEUE_MAX;
    slots[tail].type       = type;
    slots[tail].durationMs = durationMs;
    slots[tail].text[0]    = '\0';
    if (text) {
      strncpy(slots[tail].text, text, sizeof(slots[tail].text) - 1);
      slots[tail].text[sizeof(slots[tail].text) - 1] = '\0';
    }
    count++;
    return true;
  }

  DisplaySlot& front() { return slots[head]; }

  void pop() {
    if (count == 0) return;
    head = (head + 1) % TM1637D_QUEUE_MAX;
    count--;
  }

  void clear() { head = 0; count = 0; }
};

// ---------------------------------------------------------------------------
// SlotProviderFn — callback type for external slot providers
// Registered via tm1637DisplayAddSlotProvider().
// ---------------------------------------------------------------------------
using SlotProviderFn = void (*)(SlotQueue&);

// ---------------------------------------------------------------------------
// Public API functions (implemented in usermod_tm1637_display.cpp)
// ---------------------------------------------------------------------------

// Show a 4-character message on the TM1637 display immediately.
// durationMs=0 uses the configured slot-duration-ms value.
bool tm1637DisplayShowMessage(const char* msg, uint16_t durationMs = 0);

// Register a callback invoked at the start of every display cycle.
// Returns false when the provider table is full (max 4 providers).
bool tm1637DisplayAddSlotProvider(SlotProviderFn fn);

// Return the configured slot duration so providers can size their slots
// consistently with the rest of the display cycle.
uint16_t tm1637DisplayGetSlotDurationMs();

#endif // USERMOD_TM1637_DISPLAY
