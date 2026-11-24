"""Pursuit model for the stance charge run (gem_melee S24, intercept lead): where the run order is aimed and when it is re-aimed.

Not the dll's code: a model of its rules, used to pick the numbers that went into src\\stance\\stance.c ([Charge] LeadSeconds,
LeadKeep, LeadMaxFraction) and the re-aim rule. What the model assumes is listed here so its limits are plain:
  charger   runs at REAL units/s (100 / 120 / 135 are tried; the dll assumes LeadRunSpeed 120) to its ordered goal; a new order
            while it runs costs PENALTY s standing (0.2 / 0.4 / 0.6: the unit stops and re-paths, MeleeStance 0.7.13 note; the
            real cost is not known); inside CLOSE units the engine's melee order takes over, at the first look GATE s after
            the last order
  looks     every SCAN s (the weapon-pick scan), a re-aim not sooner than GATE s after the last order ([Charge] RefreshMs),
            when the test point is more than max(40, min(0.4 x distance, 200)) from the ordered goal (0.1.1 rule)
  velocity  one position sample per look that is at least 250 ms after the last one, looks 0.1-0.6 s apart; averaged with the
            one before; a sample slower than 15 units/s or faster than 400 clears it, a turn of more than 45 degrees starts the
            heading over, nothing for 1.5 s clears it; used from the second moving sample on
  aim       'cur'  = the enemy's position (0.1.1)
            lead   = position + velocity x t, t = meeting time for a runner at 120, at most H s and at most KEEP x the time the
                     enemy has kept its heading; the lead at most FRAC x the distance
  re-aim test point
            'fresh' = the point a new order would get now
            'pred'  = what the running order predicted: position + velocity x (the order's lead time - time since the order),
                      the plain position once that is used up (an unled order: the position, as 0.1.1)   [0.1.2]
            'hybrid' = 'pred' while the order's lead time lasts, 'fresh' once it is used up or for an unled order: a led
                      order lasts at most its lead time (0.1.2 in game: the unit ran on to a goal the enemy had passed until
                      the enemy was 200 beyond it, Alex: "he only updates his pos once he gets to the original location")
  stall     as the dll: looked at a second apart once the order is 1.5 s old; the unit moved less than 10 and the distance did
            not close by 10, twice in a row, and 2 s since the order ([Charge] CooldownMs): re-ordered straight at the enemy,
            and no lead for 15 s (a unit waiting at its goal for an enemy that walks onto it is not stalled)
  no terrain, no pathing, no draw phase; the distance is the true one (the dll uses the unit's sensor record)

usage: python lead_sim.py            table for the chosen numbers
       python lead_sim.py --grid     every parameter set, ranked
"""
import math
import random
import sys

ASSUMED, CLOSE, GATE, SCAN, DT = 120.0, 200.0, 0.4, 0.1, 0.05
MINSPEED, MAXSPEED, VEL_DT, STALE = 30.0, 400.0, 0.25, 1.5


def thr(d):
    return max(40.0, min(0.4 * d, 200.0))


class Vel:
    """the dll's velocity table entry for one target"""
    def __init__(self):
        self.t = None; self.x = self.y = 0.0; self.vx = self.vy = 0.0; self.n = 0; self.since = 0.0

    def note(self, now, x, y):
        if self.t is None:
            self.t, self.x, self.y, self.since = now, x, y, now
            return
        dt = now - self.t
        if dt < VEL_DT - 1e-9:
            return
        nvx, nvy = (x - self.x) / dt, (y - self.y) / dt
        sp = math.hypot(nvx, nvy)
        if dt > STALE or sp < MINSPEED / 2 or sp > MAXSPEED:
            self.vx = self.vy = 0.0; self.n = 0
        elif self.n == 0 or self.vx * nvx + self.vy * nvy < 0.7071 * math.hypot(self.vx, self.vy) * sp:
            self.vx, self.vy, self.n, self.since = nvx, nvy, 1, self.t
        else:
            self.vx, self.vy, self.n = 0.5 * (self.vx + nvx), 0.5 * (self.vy + nvy), self.n + 1
        self.t, self.x, self.y = now, x, y

    def known(self, now):
        return self.n >= 2 and self.t is not None and now - self.t <= STALE and math.hypot(self.vx, self.vy) >= MINSPEED


def lead_point(cx, cy, ex, ey, vel, now, h, keep, frac):
    """(x, y, lead time in s); lead time 0 = the enemy's position"""
    if h <= 0 or not vel.known(now):
        return ex, ey, 0.0
    rx, ry = ex - cx, ey - cy
    v2 = vel.vx ** 2 + vel.vy ** 2
    a, b, c = v2 - ASSUMED ** 2, 2 * (rx * vel.vx + ry * vel.vy), rx * rx + ry * ry
    t = -1.0
    if abs(a) < 1.0:
        if b < -1e-3:
            t = -c / b
    else:
        disc = b * b - 4 * a * c
        if disc >= 0:
            q = math.sqrt(disc)
            t1, t2 = (-b - q) / (2 * a), (-b + q) / (2 * a)
            t = t1 if t1 > 0 and (t2 <= 0 or t1 < t2) else t2
    if not t > 0 or t > h:
        t = h
    if keep > 0:
        t = min(t, keep * (now - vel.since))
    ll, cap = math.sqrt(v2) * t, frac * math.sqrt(c)
    if ll > cap:
        t *= cap / ll; ll = cap
    if ll < 1.0:
        return ex, ey, 0.0
    return ex + vel.vx * t, ey + vel.vy * t, t


# ------------------------------------------------------------------------------------------------ enemy movement patterns
def steady(angle_deg, speed):
    def make(rng):
        a = math.radians(angle_deg)
        return lambda t: (speed * math.cos(a), speed * math.sin(a))
    return make


def dash_stop(speed=100.0, run=(1.0, 3.0), pause=(1.0, 4.0)):
    def make(rng):
        st = {"until": 0.0, "v": (0.0, 0.0), "moving": False}
        def step(t):
            if t >= st["until"]:
                st["moving"] = not st["moving"]
                if st["moving"]:
                    a = rng.uniform(0, 2 * math.pi)
                    st["v"] = (speed * math.cos(a), speed * math.sin(a)); st["until"] = t + rng.uniform(*run)
                else:
                    st["v"] = (0.0, 0.0); st["until"] = t + rng.uniform(*pause)
            return st["v"]
        return step
    return make


def waypoints(speed=100.0, leg=(3.0, 8.0), turn=120.0):
    def make(rng):
        st = {"until": 0.0, "a": rng.uniform(0, 2 * math.pi)}
        def step(t):
            if t >= st["until"]:
                st["a"] += math.radians(rng.uniform(-turn, turn)); st["until"] = t + rng.uniform(*leg)
            return speed * math.cos(st["a"]), speed * math.sin(st["a"])
        return step
    return make


# angle 0 = straight away from the charger, 180 = straight at it
SCEN = [
    ("across 90deg, walk 60", steady(90, 60)),
    ("across 90deg, run 100", steady(90, 100)),
    ("away at 45deg, walk 60", steady(45, 60)),
    ("fleeing straight, walk 60", steady(0, 60)),
    ("towards at 135deg, run 100", steady(135, 100)),
    ("head on, run 100", steady(180, 100)),
    ("head on, run 140", steady(180, 140)),
    ("towards at 135deg, run 140", steady(135, 140)),
    ("dash 1-3 s / stop 1-4 s", dash_stop()),
    ("dash 0.5-1.5 / stop 1-3 s", dash_stop(100.0, (0.5, 1.5), (1.0, 3.0))),
    ("runs legs 3-8 s, turns", waypoints()),
    ("walks legs 3-6 s, turns", waypoints(60.0, (3.0, 6.0))),
    ("stands, short shuffles", dash_stop(80.0, (0.3, 0.8), (2.0, 4.0))),
]


def run(make, d0, penalty, strat, seed, real=120.0, tmax=60.0):
    """strat = None ('cur') or (h, keep, frac, mode). Returns (seconds until inside CLOSE or tmax, re-aims)."""
    rng = random.Random(seed)
    step = make(rng)
    srng = random.Random(seed ^ 0x5EED)
    cx, cy, ex, ey = 0.0, 0.0, float(d0), 0.0
    vel = Vel()
    t = -1.0                                             # the enemy was being looked at for a second before the first order
    next_look = t
    while t < 0:
        if t >= next_look:
            vel.note(t, ex, ey); next_look = t + srng.uniform(0.1, 0.6)
        vx, vy = step(t); ex += vx * DT; ey += vy * DT; t += DT
    t = 0.0
    gx = gy = 0.0
    have, ot, olead, wait, reaims, at_goal, hold, next_scan = False, -9.0, 0.0, 0.0, 0, None, -9.0, 0.0
    stalls, chk_t, chk_x, chk_y, chk_d = 0, 0.0, 0.0, 0.0, float(d0)
    while t < tmax:
        if t >= next_look:
            vel.note(t, ex, ey); next_look = t + srng.uniform(0.1, 0.6)
        if t >= next_scan:
            next_scan = t + SCAN
            d = math.hypot(ex - cx, ey - cy)
            if d <= CLOSE and (not have or t - ot >= GATE):
                return t, reaims
            if strat is None or t < hold:
                nx, ny, nl = ex, ey, 0.0
            else:
                nx, ny, nl = lead_point(cx, cy, ex, ey, vel, t, strat[0], strat[1], strat[2])
            if not have:
                gx, gy, olead, have, ot = nx, ny, nl, True, t
                stalls, chk_t, chk_x, chk_y, chk_d = 0, t, cx, cy, d
            elif t - ot >= GATE:
                rem = olead - (t - ot)
                if strat is not None and strat[3] in ("pred", "hybrid") and olead > 0 and rem > 0:
                    px, py = (ex + vel.vx * rem, ey + vel.vy * rem) if vel.known(t) else (ex, ey)
                elif strat is not None and strat[3] == "pred":
                    px, py = ex, ey
                else:
                    px, py = nx, ny
                if math.hypot(px - gx, py - gy) > thr(d):
                    if at_goal is None:
                        wait = penalty
                    gx, gy, olead, ot, at_goal = nx, ny, nl, t, None
                    reaims += 1
                    stalls, chk_t, chk_x, chk_y, chk_d = 0, t, cx, cy, d
                elif t - ot >= 1.5 and t - chk_t >= 1.0:
                    stalls = 0 if math.hypot(cx - chk_x, cy - chk_y) >= 10.0 or d < chk_d - 10.0 else stalls + 1
                    chk_t, chk_x, chk_y, chk_d = t, cx, cy, d
                    if stalls >= 2 and t - ot >= 2.0:
                        gx, gy, olead, ot, hold, at_goal = ex, ey, 0.0, t, t + 15.0, None
                        reaims += 1
                        stalls = 0
        if wait > 0:
            wait -= DT
        else:
            dx, dy = gx - cx, gy - cy
            dg = math.hypot(dx, dy)
            if dg > 1e-6:
                mv = min(real * DT, dg)
                cx += dx / dg * mv; cy += dy / dg * mv
            if dg <= real * DT and at_goal is None:
                at_goal = t
        vx, vy = step(t)
        ex += vx * DT; ey += vy * DT
        t += DT
    return tmax, reaims


def table(strats, penalties=(0.2, 0.4, 0.6), dists=(600, 1000, 1500), reals=(100.0, 120.0, 135.0), trials=12):
    res = {}
    for name, make in SCEN:
        for sname, strat in strats:
            tt = rr = 0.0; n = miss = 0
            for p in penalties:
                for d0 in dists:
                    for real in reals:
                        for k in range(trials):
                            t, r = run(make, d0, p, strat, seed=k * 7919 + d0, real=real)
                            tt += t; rr += r; n += 1; miss += t >= 60.0
            res[(name, sname)] = (tt / n, rr / n, miss / n)
    return res


CHOSEN = ("H3 k.5 hybrid", (3.0, 0.5, 0.5, "hybrid"))


def main():
    if "--grid" in sys.argv:
        strats = [("cur", None)]
        for mode in ("fresh", "pred", "hybrid"):
            for h in (2.0, 3.0, 4.0):
                for keep in (0.0, 0.5, 1.0):
                    strats.append(("H%.0f k%.1f %s" % (h, keep, mode), (h, keep, 0.5, mode)))
        res = table(strats, trials=6)
        names = [s for s, _ in strats]
        print("%-16s %7s %7s %8s   time to get inside %d units relative to 'cur' (mean over the scenarios, worst one), re-aims relative"
              % ("aim", "mean", "worst", "re-aims", CLOSE))
        rows = []
        for s in names:
            rel = [res[(n, s)][0] / res[(n, "cur")][0] for n, _ in SCEN]
            ra = sum(res[(n, s)][1] for n, _ in SCEN) / sum(res[(n, "cur")][1] for n, _ in SCEN)
            rows.append((sum(rel) / len(rel), max(rel), ra, s))
        for m, w, ra, s in sorted(rows):
            print("%-16s %7.3f %7.3f %8.2f" % (s, m, w, ra))
        return
    strats = [("cur", None), ("H3 k.5 fresh", (3.0, 0.5, 0.5, "fresh")), ("H3 k.5 pred", (3.0, 0.5, 0.5, "pred")), CHOSEN,
              ("H2 k.5 hybrid", (2.0, 0.5, 0.5, "hybrid"))]
    res = table(strats)
    names = [s for s, _ in strats]
    print("seconds until inside %d units | re-aims per charge | share never inside it in 60 s   (chosen: %s)" % (CLOSE, CHOSEN[0]))
    print("%-28s" % "enemy" + "".join("%22s" % s for s in names))
    for name, _ in SCEN:
        print("%-28s" % name + "".join("%10.1f %5.1f %4.0f%%" % (res[(name, s)][0], res[(name, s)][1], 100 * res[(name, s)][2]) for s in names))


if __name__ == "__main__":
    main()
