/*
 * bus.c - 6507 bus decoding. One call = one CPU cycle.
 */
#include "bus.h"
#include "tia.h"
#include "riot.h"
#include "cart.h"

u32 a26_cycles;
int a26_stop;
u8  a26_databus;
void (*a26_input_hook)(void);
const u8 *(*a26_next_code)(u16 *pc);

u8 bus_read(u16 addr)
{
    u8 v;
    a26_cycles++;
    if (addr & 0x1000)
        v = cart_read(addr);
    else if (!(addr & 0x0080))
        v = tia_read(addr);
    else if (!(addr & 0x0200))
        v = riot.ram[addr & 0x7F];
    else
        v = riot_read(addr);
    a26_databus = v;
    return v;
}

void bus_write(u16 addr, u8 val)
{
    a26_cycles++;
    a26_databus = val;
    if (addr & 0x1000) {
        cart_write(addr, val);
    } else if (!(addr & 0x0080)) {
        tia_write(addr, val);
        if (cart_tia_hook) cart_tia_write(addr, val);
    } else if (!(addr & 0x0200)) {
        riot.ram[addr & 0x7F] = val;
    } else {
        riot_write(addr, val);
    }
}
