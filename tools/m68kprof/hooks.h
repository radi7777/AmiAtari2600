/* hooks.h - prototypes for the Musashi callbacks (included into every
 * Musashi source by the Makefile) */
void prof_hook(unsigned int pc);
int  prof_illegal(int opcode);
