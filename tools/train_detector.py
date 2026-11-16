#!/usr/bin/env python3
"""
train_detector.py -- train the three detector tiers and emit them as C.

WHY THIS EXISTS
---------------
Triage's engine used to burn a calibrated number of microseconds per chunk
instead of running a model. That was honest scaffolding and it is not good
enough to submit: "Innovation & AI Creativity" is a quarter of the judging,
and the project had no AI in it.

This trains three real neural networks of increasing size on MNIST, and
writes them into server/nn_weights.h as float32 arrays. The server then
does real inference -- real learned weights, real arithmetic, a real
accuracy number that can be checked against a dataset everyone knows.

WHAT THE MODEL IS, STATED PLAINLY
---------------------------------
It is a 14x14 patch classifier, scanned over a 48x48 frame as a sliding
window. That is a real detector: it reports what it found, how confident it
is, and where. It is NOT a pedestrian detector, and nothing here pretends
otherwise -- a three-layer MLP on MNIST digits is a stand-in for the vision
model a shipping system would run. What is no longer a stand-in is the
inference: the cost is real arithmetic over real weights, the per-tier
accuracy differences are measured rather than asserted, and the chunk
boundaries fall between batches of patches instead of between sleeps.

THE TIERS
---------
Three networks, deliberately different in both cost and accuracy, because
the whole point of tier selection is that paying more buys you something:

    small   196 -> 16 -> 10
    medium  196 -> 64 -> 10
    large   196 -> 128 -> 64 -> 10

and scanned at different strides, so a tier is (network, density). The
accuracy each one achieves is printed at the end and goes into the C header
as the confidence the server reports -- replacing three numbers that were
previously invented.

USAGE
-----
    python tools/train_detector.py --mnist <dir> --out server/nn_weights.h

The MNIST idx files (gzipped, as distributed) go in <dir>. Nothing is
downloaded by this script on purpose: a build step that reaches out to the
network is a build step that breaks.
"""

import argparse
import gzip
import os
import struct
import sys

import numpy as np

PATCH = 14          # patch side, so 196 inputs
FRAME = 48          # frame side the server scans
# 10 digits plus a BACKGROUND class.
#
# Without it the detector is nonsense, and the first version proved it:
# every frame came back class=5 whatever the truth was. A classifier
# trained only on digits has no way to say "nothing here" -- shown an
# empty patch it still returns its most likely digit, confidently. A
# sliding window over a frame is mostly empty, so the background patches
# drowned out the one patch that mattered.
#
# A reject class is what makes scanning meaningful: the detector can
# dismiss 99% of the frame and report the best DIGIT it found.
N_DIGITS    = 10
BG_CLASS    = 10
N_CLASSES   = 11
RNG = np.random.default_rng(20261116)

#
# STRIDES ARE CHOSEN FROM MEASUREMENT, NOT TASTE.
#
# On the QNX target a patch costs about 2us at the small net, 8us at
# medium and 25us at large. The first version of this table used strides
# 6/3/2, which put the three tiers at 72us, 1.15ms and 8.1ms -- all three
# comfortably inside a 20ms deadline, so tier selection had nothing to
# decide and the server simply always ran the big model.
#
# A deadline only means something if some tier does not fit it. These
# strides put the tiers at roughly 0.16ms, 2.6ms and 31ms, so a 20ms
# deadline rules out the large tier and a loaded machine pushes the
# scheduler down to small. Denser scanning is also the realistic knob: it
# is what you actually trade away when a detector is short of time.
#
TIERS = [
    # name,     hidden layers,  stride when scanning a frame
    ("small",   [16],           4),
    ("medium",  [64],           2),
    ("large",   [128, 64],      1),
]


# ---------------------------------------------------------------- data

def read_idx(path):
    """Read an IDX file, gzipped or not."""
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rb") as f:
        magic, = struct.unpack(">I", f.read(4))
        ndim = magic & 0xFF
        dims = struct.unpack(">" + "I" * ndim, f.read(4 * ndim))
        data = np.frombuffer(f.read(), dtype=np.uint8)
    return data.reshape(dims)


def load_mnist(d):
    def find(*names):
        for n in names:
            p = os.path.join(d, n)
            if os.path.exists(p):
                return p
        sys.exit("missing MNIST file: tried " + ", ".join(names))

    xtr = read_idx(find("train-images-idx3-ubyte.gz", "train-images-idx3-ubyte"))
    ytr = read_idx(find("train-labels-idx1-ubyte.gz", "train-labels-idx1-ubyte"))
    xte = read_idx(find("t10k-images-idx3-ubyte.gz", "t10k-images-idx3-ubyte"))
    yte = read_idx(find("t10k-labels-idx1-ubyte.gz", "t10k-labels-idx1-ubyte"))
    return xtr, ytr, xte, yte


def downsample(imgs):
    """28x28 -> 14x14 by 2x2 mean. The server works at 14x14 so that a
    patch is cheap enough for the small tier to be genuinely fast."""
    n = imgs.shape[0]
    x = imgs.reshape(n, 14, 2, 14, 2).astype(np.float32)
    return x.mean(axis=(2, 4)) / 255.0


# ------------------------------------------------------------- training

def init_params(sizes):
    """He initialisation for ReLU layers."""
    params = []
    for i in range(len(sizes) - 1):
        fan_in = sizes[i]
        w = RNG.normal(0.0, np.sqrt(2.0 / fan_in),
                       size=(sizes[i], sizes[i + 1])).astype(np.float32)
        b = np.zeros(sizes[i + 1], dtype=np.float32)
        params.append([w, b])
    return params


def forward(params, x):
    """Returns (logits, activations) with ReLU on every layer but the last."""
    acts = [x]
    h = x
    for i, (w, b) in enumerate(params):
        z = h @ w + b
        h = np.maximum(z, 0.0) if i < len(params) - 1 else z
        acts.append(h)
    return h, acts


def softmax(z):
    z = z - z.max(axis=1, keepdims=True)
    e = np.exp(z)
    return e / e.sum(axis=1, keepdims=True)


def train(sizes, xtr, ytr, xte, yte, epochs=14, batch=128, lr=0.12):
    params = init_params(sizes)
    n = xtr.shape[0]
    onehot = np.zeros((n, N_CLASSES), dtype=np.float32)
    onehot[np.arange(n), ytr] = 1.0

    for ep in range(epochs):
        order = RNG.permutation(n)
        # Simple step decay: enough to land near the usual MNIST numbers
        # without turning this script into a research project.
        step = lr * (0.6 ** (ep // 5))
        for s in range(0, n, batch):
            idx = order[s:s + batch]
            xb, yb = xtr[idx], onehot[idx]

            logits, acts = forward(params, xb)
            probs = softmax(logits)
            grad = (probs - yb) / len(idx)

            for i in range(len(params) - 1, -1, -1):
                w, b = params[i]
                a = acts[i]
                gw = a.T @ grad
                gb = grad.sum(axis=0)
                if i > 0:
                    grad = (grad @ w.T) * (acts[i] > 0)
                params[i][0] = w - step * gw
                params[i][1] = b - step * gb

        acc = accuracy(params, xte, yte)
        print(f"    epoch {ep + 1:2d}/{epochs}  test acc {acc * 100:.2f}%",
              flush=True)
    return params, accuracy(params, xte, yte)


def accuracy(params, x, y):
    logits, _ = forward(params, x)
    return float((logits.argmax(axis=1) == y).mean())


# ------------------------------------------------------- frame building

def shift_patch(flat, dx, dy):
    """Translate a 14x14 patch, zero-filling what moves in."""
    img = flat.reshape(PATCH, PATCH)
    out = np.zeros_like(img)
    xs0, xs1 = max(0, dx), min(PATCH, PATCH + dx)
    ys0, ys1 = max(0, dy), min(PATCH, PATCH + dy)
    out[ys0:ys1, xs0:xs1] = img[ys0 - dy:ys1 - dy, xs0 - dx:xs1 - dx]
    return out.ravel()


def augment_shifts(x, y, frac=0.6, amount=2):
    """Add shifted copies of a fraction of the digits.

    An MNIST classifier is not shift invariant, and a sliding window whose
    stride is 2 or 4 pixels will rarely land exactly on the object. The
    first version of this ignored that and the detector was wrong on every
    single frame -- confidently, which is worse. Teaching it a couple of
    pixels of tolerance is the difference between a detector and a
    coincidence.
    """
    n = int(len(x) * frac)
    idx = RNG.choice(len(x), size=n, replace=False)
    out = np.empty((n, x.shape[1]), dtype=np.float32)
    for k, i in enumerate(idx):
        dx = int(RNG.integers(-amount, amount + 1))
        dy = int(RNG.integers(-amount, amount + 1))
        out[k] = shift_patch(x[i], dx, dy)
    return out, y[idx]


def make_background(n):
    """Patches that look like the empty parts of a frame.

    Generated the same way build_frames() makes its background, because a
    reject class is only useful if it was trained on the thing it will
    actually be asked to reject.
    """
    noise = np.abs(RNG.normal(0.0, 0.03, size=(n, PATCH * PATCH)))
    # A fraction get a stray smudge, so "background" does not just mean
    # "almost exactly zero" and the class has to learn something.
    for k in range(0, n, 4):
        y0 = int(RNG.integers(0, PATCH - 4))
        x0 = int(RNG.integers(0, PATCH - 4))
        patch = noise[k].reshape(PATCH, PATCH)
        patch[y0:y0 + 3, x0:x0 + 3] += RNG.uniform(0.2, 0.7)
    return np.clip(noise, 0.0, 1.0).astype(np.float32)


def build_frames(xte_small, yte, count):
    """Paste one real test digit into a blank frame at a known position.

    This is what makes the server's output checkable on target: it scans
    the frame, says what it found and where, and the header carries the
    truth so an on-target test can score it.
    """
    frames = []
    for k in range(count):
        i = int(RNG.integers(0, xte_small.shape[0]))
        # Keep the digit fully inside the frame.
        # On a multiple of 4, which is the coarsest stride any tier uses,
        # so every tier CAN land on the object. Without this the small
        # tier is being asked a question it cannot answer, and the
        # accuracy figure would measure window alignment rather than the
        # network. Augmentation above covers the near-misses.
        span = (FRAME - PATCH) // 4
        ox = int(RNG.integers(0, span + 1)) * 4
        oy = int(RNG.integers(0, span + 1)) * 4
        f = np.zeros((FRAME, FRAME), dtype=np.float32)
        f[oy:oy + PATCH, ox:ox + PATCH] = xte_small[i]
        # A little background texture, so a frame is not mostly exact
        # zeros and the scan has something to reject.
        noise = RNG.normal(0.0, 0.03, size=(FRAME, FRAME)).astype(np.float32)
        f = np.clip(f + np.abs(noise), 0.0, 1.0)
        frames.append((f, int(yte[i]), ox, oy))
    return frames


# ---------------------------------------------------------------- emit

def c_float_array(name, arr):
    flat = np.asarray(arr, dtype=np.float32).ravel()
    out = [f"static const float {name}[{flat.size}] = {{"]
    line = "   "
    for v in flat:
        # %g happily prints "0" or "1" for round values, and "0f" is not a
        # valid C literal -- the suffix needs a decimal point or exponent
        # in front of it. Cheap to guarantee, and the alternative is 11000
        # compiler errors.
        lit = f"{v:.6g}"
        if "." not in lit and "e" not in lit and "E" not in lit:
            lit += ".0"
        tok = f" {lit}f,"
        if len(line) + len(tok) > 78:
            out.append(line)
            line = "   "
        line += tok
    out.append(line)
    out.append("};")
    return "\n".join(out)


def emit(path, trained, frames):
    L = []
    L.append("/*")
    L.append(" * nn_weights.h -- GENERATED by tools/train_detector.py. Do not edit.")
    L.append(" *")
    L.append(" * Three real networks trained on MNIST, their measured test accuracy,")
    L.append(" * and a handful of real frames with ground truth so the server's")
    L.append(" * output can be scored on the target rather than trusted.")
    L.append(" *")
    L.append(" * Accuracy below is measured on the 10k MNIST test set at 14x14:")
    for name, _, _, acc, _ in trained:
        L.append(f" *     {name:<7} {acc * 100:.2f}%")
    L.append(" */")
    L.append("#ifndef TRIAGE_NN_WEIGHTS_H_")
    L.append("#define TRIAGE_NN_WEIGHTS_H_")
    L.append("")
    L.append(f"#define NN_PATCH      {PATCH}")
    L.append(f"#define NN_PATCH_LEN  {PATCH * PATCH}")
    L.append(f"#define NN_FRAME      {FRAME}")
    L.append(f"#define NN_CLASSES    {N_CLASSES}")
    L.append(f"#define NN_BG_CLASS   {BG_CLASS}   /* the reject class */")
    L.append(f"#define NN_FRAMES     {len(frames)}")
    L.append("")

    max_layers = max(len(p) for _, p, _, _, _ in trained)
    max_units = 0
    for _, params, _, _, _ in trained:
        for w, _b in params:
            max_units = max(max_units, w.shape[1])
    L.append(f"#define NN_MAX_LAYERS {max_layers}")
    L.append(f"#define NN_MAX_UNITS  {max(max_units, PATCH * PATCH)}")
    L.append("")

    for name, params, stride, acc, _ in trained:
        for i, (w, b) in enumerate(params):
            L.append(c_float_array(f"nn_{name}_w{i}", w))
            L.append(c_float_array(f"nn_{name}_b{i}", b))
        L.append("")

    # Layer descriptors
    L.append("typedef struct { int in, out; const float *w, *b; } nn_layer_t;")
    L.append("typedef struct {")
    L.append("    const char       *name;")
    L.append("    int               n_layers;")
    L.append("    int               stride;    /* sliding-window stride */")
    L.append("    unsigned          accuracy;  /* measured, x10 (0..1000) */")
    L.append("    const nn_layer_t *layers;")
    L.append("} nn_net_t;")
    L.append("")

    for name, params, stride, acc, _ in trained:
        L.append(f"static const nn_layer_t nn_{name}_layers[] = {{")
        for i, (w, b) in enumerate(params):
            L.append(f"    {{ {w.shape[0]}, {w.shape[1]}, "
                     f"nn_{name}_w{i}, nn_{name}_b{i} }},")
        L.append("};")
    L.append("")

    L.append("/* Indexed by triage_tier_t: [0] is unused (TRIAGE_TIER_NONE). */")
    L.append("static const nn_net_t nn_nets[] = {")
    L.append('    { "none", 0, 0, 0, NULL },')
    for name, params, stride, acc, _ in trained:
        L.append(f'    {{ "{name}", {len(params)}, {stride}, '
                 f'{int(round(acc * 1000))}, nn_{name}_layers }},')
    L.append("};")
    L.append("")

    # Frames + ground truth
    L.append("/* Real MNIST test digits pasted into frames at known positions. */")
    for k, (f, label, ox, oy) in enumerate(frames):
        L.append(c_float_array(f"nn_frame{k}", f))
    L.append("")
    L.append("typedef struct {")
    L.append("    const float *pixels;")
    L.append("    int          label, x, y;")
    L.append("} nn_frame_t;")
    L.append("")
    L.append("static const nn_frame_t nn_frames[NN_FRAMES] = {")
    for k, (f, label, ox, oy) in enumerate(frames):
        L.append(f"    {{ nn_frame{k}, {label}, {ox}, {oy} }},")
    L.append("};")
    L.append("")
    L.append("#endif /* TRIAGE_NN_WEIGHTS_H_ */")

    with open(path, "w", newline="\n") as fh:
        fh.write("\n".join(L) + "\n")


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mnist", required=True, help="dir with MNIST idx files")
    ap.add_argument("--out", default="server/nn_weights.h")
    ap.add_argument("--frames", type=int, default=6)
    ap.add_argument("--epochs", type=int, default=14)
    args = ap.parse_args()

    print("loading MNIST ...", flush=True)
    xtr, ytr, xte, yte = load_mnist(args.mnist)
    xtr_s = downsample(xtr).reshape(len(xtr), -1)
    xte_s = downsample(xte).reshape(len(xte), -1)
    print(f"  train {xtr_s.shape}  test {xte_s.shape}", flush=True)

    # Mix in background examples, a fifth of the digit count, so the
    # detector can reject the empty parts of a frame.
    xa, ya = augment_shifts(xtr_s, ytr)
    xtr_s = np.vstack([xtr_s, xa])
    ytr = np.concatenate([ytr, ya])
    print(f"  + shift augmentation: train {xtr_s.shape}", flush=True)

    n_bg_tr = len(xtr_s) // 5
    n_bg_te = len(xte_s) // 5
    xtr_s = np.vstack([xtr_s, make_background(n_bg_tr)])
    ytr = np.concatenate([ytr, np.full(n_bg_tr, BG_CLASS, dtype=ytr.dtype)])
    xte_bg = make_background(n_bg_te)
    xte_all = np.vstack([xte_s, xte_bg])
    yte_all = np.concatenate([yte, np.full(n_bg_te, BG_CLASS, dtype=yte.dtype)])
    print(f"  + background: train {xtr_s.shape} test {xte_all.shape}",
          flush=True)

    trained = []
    for name, hidden, stride in TIERS:
        sizes = [PATCH * PATCH] + hidden + [N_CLASSES]
        params_n = sum(w.size + b.size for w, b in init_params(sizes))
        print(f"\ntraining {name}: {' -> '.join(map(str, sizes))} "
              f"({params_n} params, stride {stride})", flush=True)
        params, acc = train(sizes, xtr_s, ytr, xte_all, yte_all,
                            epochs=args.epochs)
        # Digit-only accuracy is the number that matters for the detector:
        # rejecting background is easy and would flatter the figure.
        acc_digits = accuracy(params, xte_s, yte)
        macs = sum(w.shape[0] * w.shape[1] for w, _ in params)
        print(f"  {name}: test accuracy {acc * 100:.2f}% "
              f"(digits only {acc_digits * 100:.2f}%), "
              f"{macs} MACs per patch", flush=True)
        trained.append((name, params, stride, acc_digits, macs))

    frames = build_frames(downsample(xte), yte, args.frames)

    emit(args.out, trained, frames)
    size = os.path.getsize(args.out)
    print(f"\nwrote {args.out} ({size // 1024} KiB)")
    print("\nper-tier summary (this is what the server will report):")
    for name, params, stride, acc, macs in trained:
        patches = ((FRAME - PATCH) // stride + 1) ** 2
        print(f"  {name:<7} acc {acc * 100:5.2f}%  stride {stride}  "
              f"{patches:4d} patches x {macs:6d} MACs = "
              f"{patches * macs / 1e6:7.2f}M MACs per frame")


if __name__ == "__main__":
    main()
