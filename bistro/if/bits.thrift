namespace cpp facebook.bistro
namespace py facebook.bistro.bits

enum BistroTaskStatusBits {
  UNSTARTED = 1,

  RUNNING = 2,
  DONE = 4,
  INCOMPLETE = 8,
  FAILED = 16,
  ERROR = 32,

  USES_BACKOFF = 64,
  DOES_NOT_ADVANCE_BACKOFF = 512,

  OVERWRITEABLE = 1024,

  CURRENT_STATUS_MASK = 1663,  // OR of the preceding bits

  AVOIDED = 128,
  DISABLED = 256,

  CAN_RUN_MASK = 384,  // OR of the preceding bits
}
