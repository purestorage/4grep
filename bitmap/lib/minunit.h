/** MinUnit
 * As taken from http://www.jera.com/techinfo/jtns/jtn002.html
 * License: You may use the code in this tech note for any purpose, with
 * the understanding that it comes with NO WARRANTY. */
#define mu_assert(message, test) do { if (!(test)) return message; } while (0)
#define mu_run_test(test) do { char *message = test(); tests_run++; \
                                  if (message) return message; } while (0)
extern int tests_run;
