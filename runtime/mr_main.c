/* moon-river-game: the runtime is libmoonriver.so beside this program (boot.c mr_host_main), so the whole of it can be
 * prebuilt; the launcher starts this when it cannot load the library itself. */
int mr_host_main(int argc, char **argv);
int main(int argc, char **argv) { return mr_host_main(argc, argv); }
