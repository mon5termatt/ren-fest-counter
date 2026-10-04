# Ren Fest Counter — Power Plan

Rough runtime estimates for battery operation at a festival day
(**~10 am–9 pm ≈ 11 hours** continuous).

## Measured draws (5 V)

| Condition | Current |
|-----------|---------|
| One half, min brightness | 0.2 A |
| Full setup, min brightness | ~0.4 A (estimated 2× half) |
| Full setup, full brightness (normal content) | 1.1 A |
| Checkerboard test, full bright | 2.0 A |
| All LEDs on, full bright | 3.1 A |

ESP32 / Wi‑Fi overhead is included in these meter readings.

## Power banks

Capacity labels are at **~3.7 V cell** voltage. Usable energy at **5 V USB**
after boost (~85% efficient):

| Bank | Label | Cell energy | ≈ Usable at 5 V |
|------|-------|-------------|-----------------|
| Small | 9000 mAh | ~33 Wh | ~5.6 Ah |
| Baseus Amblight | 30 000 mAh 65 W | **111 Wh** (rated) | ~18.9 Ah |

111 Wh = 30 000 mAh × 3.7 V — same figure the pack claims. At 5 V with ~85%
boost: `111 × 0.85 / 5 ≈ 18.9 Ah`.

[Baseus Amblight 65W 30000mAh](https://www.baseus.com/products/amblight-power-bank-65w-30000mah)

## Runtime estimates

| Load | 9000 mAh | One 30 K (111 Wh) | Two 30 K (sequential) |
|------|----------|-------------------|------------------------|
| Full setup, min (0.4 A) | ~14 h | ~47 h | ~94 h |
| Full setup, high (1.1 A) | ~5 h | ~17 h | ~34 h |
| Checkerboard 2.0 A | ~2.8 h | ~9.5 h | ~19 h |
| All-on 3.1 A | ~1.8 h | ~6 h | ~12 h |

Naive “mAh ÷ mA” without the 3.7→5 V conversion overstates runtime by ~1.5–1.6×.

## Festival day (10 am–9 pm)

**11 hours** on one 30 000 mAh bank:

| Brightness | ~Draw | Used in 11 h | Headroom on one 30 K |
|------------|-------|--------------|----------------------|
| Min | 0.4 A | ~4.4 Ah | Plenty (~¼ of bank) |
| Medium | 0.8 A | ~8.8 Ah | Comfortable (~½) |
| High | 1.1 A | ~12 Ah | OK (~⅔) |
| All-on stress | 3.1 A | ~34 Ah | **Not viable** on one bank |

## Recommended kit

- **Two Baseus 30 000 mAh** banks on hand.
- **Bank A** — counter only (dedicated cable; don’t share with phone).
- **Bank B** — phone + spare / overnight top-up.

A full phone charge is roughly **10–20 Wh** (~2–4 Ah at 5 V). Bank B can
refill a phone several times and still leave reserve.

## Tips

- Prefer **min–medium brightness** outdoors only as needed; blank/night mode
  when idle.
- Avoid long **all-on / full-bright** test patterns on battery.
- Use a short, thick USB cable; thin cables drop voltage at 1 A+.
- Watch the bank’s overload cutoff if something spikes near 3 A.
- Top up overnight between fest days if running high brightness all day.
