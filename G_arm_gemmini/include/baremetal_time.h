#ifndef MNIST_BAREMETAL_TIME_H
#define MNIST_BAREMETAL_TIME_H
/* The synchronous queue transport supplies its own bounded polling.
 * IREE timers are unused in this bare-metal application. These definitions
 * select the same no-clock/no-sleep behavior as IREE's Generic fallback. */
#ifndef IREE_TIME_NOW_FN
#define IREE_TIME_NOW_FN { return 0; }
#endif
#define IREE_WAIT_UNTIL_FN(deadline_ns) ((void)(deadline_ns), true)
#endif
