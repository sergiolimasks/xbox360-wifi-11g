/*
 * xbox360-wifi-11g — make the Xbox 360 S/E internal Wi-Fi receive at full speed (802.11g instead of 11n).
 *
 * Problem: associated in 802.11n, many routers send to the 360 at the lowest PHY rate (6 Mb/s), so the
 * console RECEIVES only ~4.3 Mb/s (it still SENDS at ~32 Mb/s). Measured both from a homebrew XEX and with
 * Aurora's FTP, without any homebrew running. Associated in 802.11g the router adapts normally (18-24 Mb/s
 * PHY) and the console receives 11-13 Mb/s.
 *
 * Fix: the system Wi-Fi driver ("Omni", Marvell, inside the kernel) decides to use 11n from bit 0x800 of the
 * adapter capabilities in its global state. We find that state in memory, clear the bit and ask the driver
 * to reconfigure (re-applying the TX power level through XNetSetOpt does it), so it re-associates in 11g.
 * Only memory is changed: it lasts until the console is turned off. Nothing is written to flash or settings.
 *
 * Requirements: a title/XEX built with the Xbox 360 XDK, running on a console where titles can read and write
 * kernel data (RGH/JTAG). Call wifi_force_g() after XNetStartup/WSAStartup, once the console has an IP.
 *
 * Tested on: kernel 17559, Xbox 360 S internal Wi-Fi, a Wi-Fi 6 router on 2.4 GHz.
 * License: MIT (see LICENSE). Part of the "orbita" project (xCloud client for the Xbox 360).
 */
#include <xtl.h>
#include <stdint.h>

BOOLEAN __stdcall MmIsAddressValid(PVOID p);   /* xboxkrnl export (xboxkrnl.lib) */

/* XNetGetOpt/XNetSetOpt option that the Omni driver answers with its TX power level (NDIS
 * OID_802_11_TX_POWER_LEVEL); setting it makes the driver reconfigure, i.e. re-associate */
#define OPT_TX_POWER   0x0D010205u

/* offsets inside the Omni driver global state (same in the debug kernel and in 17559) */
#define OMNI_DEVTYPE   0x008   /* small integer, 0xc for the internal 360 S adapter */
#define OMNI_CAPS      0x00c   /* adapter capabilities; 0x800 = 802.11n */
#define OMNI_ASSOC     0x478   /* association state; 0x4000 = associated with HT (11n) */
#define CAPS_11N       0x800u
#define ASSOC_HT       0x4000u

/*
 * Finds the Omni driver global state without hard-coded kernel addresses (works across kernel versions):
 * the driver's get-option handler compares the option id with 0x2B00 ("cmplwi cr6, r4, 0x2B00" =
 * 0x2B042B00) and, a few instructions later, calls Omni80211GetCurrentRate, which starts by loading the
 * global state address with "lis rX, hi" + "addi rY, rX, lo". We decode those two instructions.
 */
static uint32_t omni_find(void)
{
    uint32_t a;
    for (a = 0x80040000u; a < 0x80200000u; a += 4) {
        const uint32_t *w = (const uint32_t *)(uintptr_t)a;
        uint32_t i, f, hi = 0, rhi = 99;
        if ((a & 0xfff) == 0 && !MmIsAddressValid((PVOID)(uintptr_t)a)) { a += 0xffc; continue; }
        if (*w != 0x2B042B00u) continue;                      /* cmplwi cr6, r4, 0x2B00 */
        for (i = 1; i < 6 && (w[i] >> 26) != 18; i++) {}       /* next "bl" */
        if (i == 6 || (w[i] & 3) != 1) continue;
        f = a + i * 4 + (uint32_t)(((int32_t)(w[i] << 6)) >> 6 & ~3);   /* bl target */
        if (!MmIsAddressValid((PVOID)(uintptr_t)f)) continue;
        w = (const uint32_t *)(uintptr_t)f;
        for (i = 0; i < 16; i++) {
            uint32_t op = w[i] >> 26, rd = (w[i] >> 21) & 31, ra = (w[i] >> 16) & 31;
            if (op == 15 && ra == 0) { hi = w[i] << 16; rhi = rd; }                                       /* lis  */
            else if (op == 14 && ra == rhi) return hi + (uint32_t)(int32_t)(int16_t)(w[i] & 0xffff);   /* addi */
        }
    }
    return 0;
}

/* 1 = now in 11g (or already was), -1 = not done (driver not found / state didn't match / still 11n) */
int wifi_force_g(void)
{
    uint32_t g, caps, st, tx = 0, type;
    DWORD n = sizeof(tx);
    int i;

    if (!(XNetGetEthernetLinkStatus() & XNET_ETHERNET_LINK_WIRELESS))
        return -1;   /* cable: nothing to do */
    g = omni_find();
    if (!g || !MmIsAddressValid((PVOID)(uintptr_t)g) || !MmIsAddressValid((PVOID)(uintptr_t)(g + OMNI_ASSOC)))
        return -1;
    type = *(volatile uint32_t *)(uintptr_t)(g + OMNI_DEVTYPE);
    caps = *(volatile uint32_t *)(uintptr_t)(g + OMNI_CAPS);
    if (type < 4 || type > 0x20 || (caps & ~0xfffu))
        return -1;   /* doesn't look like the Omni state: touch nothing */
    if (!(caps & CAPS_11N))
        return 1;    /* already done (e.g. earlier in this power cycle) */

    if (XNetGetOpt(OPT_TX_POWER, (BYTE *)&tx, &n) != 0 || !tx)
        tx = 100;
    *(volatile uint32_t *)(uintptr_t)(g + OMNI_CAPS) = caps & ~CAPS_11N;
    XNetSetOpt(OPT_TX_POWER, (const BYTE *)&tx, sizeof(tx));   /* reconfigure -> re-associate */

    for (i = 0; i < 100; i++) {   /* up to 10 s to come back */
        XNADDR xa;
        DWORD s;
        Sleep(100);
        st = *(volatile uint32_t *)(uintptr_t)(g + OMNI_ASSOC);
        s = XNetGetTitleXnAddr(&xa);
        if (i >= 10 && !(st & ASSOC_HT) && s != XNET_GET_XNADDR_PENDING && !(s & XNET_GET_XNADDR_NONE) &&
            (XNetGetEthernetLinkStatus() & XNET_ETHERNET_LINK_ACTIVE))
            break;
    }
    return (st & ASSOC_HT) ? -1 : 1;
}
