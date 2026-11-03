#!/usr/bin/env python3
"""Emit a quantized (int8) StableHLO version of the MNIST CNN.

Two things make this different from the float exporter next to it:

1. **Gemmini is int8.** `elem_t` is int8_t in gemmini_params.h; the systolic
   array has no floating-point path. A float model can never run on it.

2. **Convolutions become im2col + matmul.** The graph is rewritten so that the
   only "heavy" ops left are `stablehlo.dot_general`, i.e. plain matrix
   multiplies. That is deliberate: matmul is what the systolic array does, and
   a vmfb dispatch whose body is one matmul is the thing step E already knows
   how to redirect to Gemmini.

Quantization scheme
-------------------
Per-tensor symmetric: one scale per tensor, zero point 0.

    q(x) = round(x / s),  s = max|x| / 127,  x ~= q * s

Zero point 0 is not an arbitrary simplification: it makes ReLU a plain
`max(q, 0)` and MaxPool a plain integer max, because the ordering of the
quantized values then matches the ordering of the real ones. With a nonzero
zero point both would need corrections.

Requantization is done in **integer arithmetic**, not float:

    acc_i32 = dot(a_i8, w_i8) + bias_i32
    y_i8    = clamp((acc_i32 * M + (1 << (shift-1))) >> shift, -127, 127)
    M       = round(s_in * s_w / s_out * 2^shift)

The multiplier/shift form is what real quantized runtimes (and Gemmini's own
ACC_SCALE) use. Doing it in integers is also what makes the numpy reference
and IREE bit-exact -- with float rounding they would differ by 1 LSB near .5
and the comparison would be mushy.

Layout, as in the float exporter: NCHW in the ONNX file, NHWC in the MLIR,
with one explicit transpose before the flatten.
"""

import pathlib
import sys

import numpy as np
import onnx
from onnx import numpy_helper

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from onnx_to_stablehlo import Emitter, make_input  # noqa: E402

F32 = np.float32
QMAX = 127


def tensor_type(shape, dtype="f32"):
    if len(shape) == 0:
        return f"tensor<{dtype}>"
    return "tensor<" + "x".join(str(d) for d in shape) + "x" + dtype + ">"


def dense_float(arr):
    a = np.asarray(arr)
    if a.ndim == 0:
        return repr(float(a))
    if a.ndim == 1:
        return "[" + ", ".join(repr(float(x)) for x in a) + "]"
    return "[" + ", ".join(dense_float(s) for s in a) + "]"


def dense_int(arr):
    """Nested integer literal. `dense()` from the float exporter formats
    everything as floats, which would turn 127 into 127.0 in an i8 tensor."""
    a = np.asarray(arr)
    if a.ndim == 0:
        return str(int(a))
    if a.ndim == 1:
        return "[" + ", ".join(str(int(x)) for x in a) + "]"
    return "[" + ", ".join(dense_int(s) for s in a) + "]"


# ---------------------------------------------------------------------------
# Scales and fixed-point multipliers
# ---------------------------------------------------------------------------

def choose_scale(absmax, floor=1e-8):
    return np.float32(max(float(absmax), floor) / QMAX)


def choose_multiplier(real, max_shift=30):
    """Decompose a positive real into M * 2^-shift with M a positive int32."""
    real = float(real)
    assert real > 0
    shift = 0
    while real * (1 << (shift + 1)) < (1 << max_shift):
        shift += 1
    m = int(round(real * (1 << shift)))
    assert 0 < m < (1 << 31), (real, m, shift)
    return m, shift


def requant_np(acc, M, shift):
    """The exact integer requantize the MLIR performs."""
    acc = acc.astype(np.int64)
    r = (acc * np.int64(M) + (np.int64(1) << (shift - 1))) >> shift
    return np.clip(r, -QMAX, QMAX).astype(np.int8)


# ---------------------------------------------------------------------------
# Emitting
# ---------------------------------------------------------------------------

class QEmitter(Emitter):
    def _const(self, arr, dtype):
        v = self.val()
        np_dtype = {"i8": np.int8, "i32": np.int32, "i64": np.int64,
                    "f32": np.float32}[dtype]
        a = np.asarray(arr, dtype=np_dtype)
        lit = dense_int(a) if dtype.startswith("i") else dense_float(a)
        self.lines.append(
            f"    {v} = stablehlo.constant dense<{lit}> : {tensor_type(a.shape, dtype)}")
        return v

    def const_i8(self, arr):
        return self._const(arr, "i8")

    def const_i32(self, arr):
        return self._const(arr, "i32")

    def const_i64(self, arr):
        return self._const(arr, "i64")

    def const_f32(self, arr):
        return self._const(arr, "f32")

    def cast(self, v, shape, from_t, to_t):
        return self.op(f'"stablehlo.convert"({v}) : '
                       f'({tensor_type(shape, from_t)}) -> {tensor_type(shape, to_t)}')

    def splat(self, value, shape, dtype):
        """A rank-N tensor filled with one value, via a (1,..,1) constant.

        Rank-0 constants plus an empty broadcast_dimensions list are legal but
        fiddly to spell; a (1,..,1) constant broadcast over every dimension is
        unambiguous and IREE folds it away immediately.
        """
        unit = (1,) * len(shape)
        c = self._const(np.full(unit, value), dtype)
        dims = ", ".join(str(i) for i in range(len(shape)))
        return self.op(
            f'"stablehlo.broadcast_in_dim"({c}) '
            f'{{broadcast_dimensions = array<i64: {dims}>}} '
            f': ({tensor_type(unit, dtype)}) -> {tensor_type(shape, dtype)}')

    def row_broadcast(self, row, shape, dtype):
        """Broadcast a [1, N] row across the batch dimension of an [M, N] tensor."""
        c = self._const(np.asarray(row).reshape(1, -1), dtype)
        dims = ", ".join(str(i) for i in range(len(shape)))
        return self.op(
            f'"stablehlo.broadcast_in_dim"({c}) '
            f'{{broadcast_dimensions = array<i64: {dims}>}} '
            f': ({tensor_type((1, np.asarray(row).size), dtype)}) -> '
            f'{tensor_type(shape, dtype)}')


def emit_int8(model, calib):
    """calib: onnx tensor name -> max|x|, measured on one float run."""
    graph = model.graph
    inits = {i.name: numpy_helper.to_array(i) for i in graph.initializer}
    for node in graph.node:
        if node.op_type == "Constant":
            for attr in node.attribute:
                if attr.name == "value":
                    inits[node.output[0]] = numpy_helper.to_array(attr.t)

    e = QEmitter()
    e.lines.append("module {")
    e.lines.append(
        "  func.func @main(%arg0: tensor<1x28x28x1xi8>) -> tensor<1x10xf32> {")

    # env: onnx name -> (mlir value, shape); scale: onnx name -> quantization scale
    env = {graph.input[0].name: ("%arg0", (1, 28, 28, 1))}
    dtype = {graph.input[0].name: "i8"}
    scale = {graph.input[0].name: choose_scale(calib[graph.input[0].name])}
    meta = {}   # recorded for the report / reference implementation

    def im2col(x, xshape, kh, kw, oh, ow):
        """Every kernel-position patch, laid side by side.

        Result is [oh*ow, kh*kw*C] with the column index running (kh, kw, c),
        c fastest -- the same order the weights are flattened in below, which
        is what makes the matmul a convolution.
        """
        n, h, w, c = xshape
        slices = []
        for i in range(kh):
            for j in range(kw):
                sl = e.op(
                    f'"stablehlo.slice"({x}) {{'
                    f'start_indices = array<i64: 0, {i}, {j}, 0>, '
                    f'limit_indices = array<i64: {n}, {i + oh}, {j + ow}, {c}>, '
                    f'strides = array<i64: 1, 1, 1, 1>}} '
                    f': ({tensor_type(xshape, "i8")}) -> {tensor_type((n, oh, ow, c), "i8")}')
                slices.append(e.op(
                    f'"stablehlo.reshape"({sl}) : ({tensor_type((n, oh, ow, c), "i8")}) -> '
                    f'{tensor_type((oh * ow, c), "i8")}'))
        opnds = ", ".join(slices)
        types = ", ".join(tensor_type((oh * ow, c), "i8") for _ in slices)
        return e.op(
            f'"stablehlo.concatenate"({opnds}) {{dimension = 1}} '
            f': ({types}) -> {tensor_type((oh * ow, kh * kw * c), "i8")}')

    # Which Gemm feeds the LogSoftmax? There is a Relu between them, so we walk
    # back from the LogSoftmax through the elementwise ops. That layer must stay
    # in float: quantizing its output to int8 and then taking exp() would turn
    # log-softmax into noise.
    producer = {o: n for n in graph.node for o in n.output}
    LAST_FLOAT_TENSORS = set()
    for n in graph.node:
        if n.op_type == "LogSoftmax":
            t = n.input[0]
            while t in producer and producer[t].op_type in ("Relu", "Reshape"):
                t = producer[t].input[0]
            LAST_FLOAT_TENSORS.add(t)

    DDN = ('#stablehlo.dot<lhs_batching_dimensions = [], '
           'rhs_batching_dimensions = [], '
           'lhs_contracting_dimensions = [1], '
           'rhs_contracting_dimensions = [0]>')

    for node in graph.node:
        op = node.op_type
        attrs = {a.name: onnx.helper.get_attribute_value(a) for a in node.attribute}
        if op == "Constant":
            continue
        x, xshape = env[node.input[0]]
        s_in = scale[node.input[0]]

        if op == "Conv":
            n, h, w, c = xshape
            wf = inits[node.input[1]]
            bf = inits[node.input[2]].astype(F32)
            kh, kw = wf.shape[2], wf.shape[3]
            sh, sw = attrs.get("strides", [1, 1])
            assert attrs.get("pads", [0, 0, 0, 0]) == [0, 0, 0, 0], "padding not handled"
            assert attrs.get("group", 1) == 1, "groups not handled"
            assert attrs.get("dilations", [1, 1]) == [1, 1], "dilation not handled"
            oh, ow = (h - kh) // sh + 1, (w - kw) // sw + 1
            out_ch = wf.shape[0]

            s_out = choose_scale(calib[node.output[0]])
            s_w = choose_scale(np.abs(wf).max())

            # ONNX [K, C, kh, kw] -> [kh*kw*C, K] with (kh, kw, c) column order
            wt = np.transpose(wf, (2, 3, 1, 0)).reshape(kh * kw * c, out_ch)
            wq = np.clip(np.round(wt / s_w), -QMAX, QMAX).astype(np.int8)

            patches = im2col(x, xshape, kh, kw, oh, ow)
            wv = e.const_i8(wq)
            acc = e.op(
                f'"stablehlo.dot_general"({patches}, {wv}) {{dot_dimension_numbers = {DDN}}} '
                f': ({tensor_type((oh * ow, kh * kw * c), "i8")}, '
                f'{tensor_type((kh * kw * c, out_ch), "i8")}) -> '
                f'{tensor_type((oh * ow, out_ch), "i32")}')

            M, shift = choose_multiplier(float(s_in) * float(s_w) / float(s_out))
            bias_i32 = np.round(bf / (float(s_in) * float(s_w))).astype(np.int32)
            y = requantize(e, acc, (oh * ow, out_ch), bias_i32, M, shift)
            y = e.op(f'"stablehlo.reshape"({y}) : '
                     f'({tensor_type((oh * ow, out_ch), "i8")}) -> '
                     f'{tensor_type((n, oh, ow, out_ch), "i8")}')

            env[node.output[0]] = (y, (n, oh, ow, out_ch))
            dtype[node.output[0]] = "i8"
            scale[node.output[0]] = s_out
            meta[node.output[0]] = dict(kind="conv", M=M, shift=shift,
                                        mnk=(oh * ow, out_ch, kh * kw * c),
                                        s_in=float(s_in), s_w=float(s_w),
                                        s_out=float(s_out), bias=bias_i32,
                                        wq=wq, shape=(n, oh, ow, out_ch))

        elif op == "MaxPool":
            n, h, w, c = xshape
            kh, kw = attrs["kernel_shape"]
            sh, sw = attrs.get("strides", [1, 1])
            assert attrs.get("pads", [0, 0, 0, 0]) == [0, 0, 0, 0], "padding not handled"
            oh, ow = (h - kh) // sh + 1, (w - kw) // sw + 1
            neg = e.const_i8(np.array(-128, dtype=np.int8))
            y = e.op(
                f'"stablehlo.reduce_window"({x}, {neg}) ({{\n'
                f'        ^bb0(%a: tensor<i8>, %b: tensor<i8>):\n'
                f'          %m = stablehlo.maximum %a, %b : tensor<i8>\n'
                f'          stablehlo.return %m : tensor<i8>\n'
                f'        }}) {{\n'
                f'          window_dimensions = array<i64: 1, {kh}, {kw}, 1>,\n'
                f'          window_strides = array<i64: 1, {sh}, {sw}, 1>,\n'
                f'          base_dilations = array<i64: 1, 1, 1, 1>,\n'
                f'          window_dilations = array<i64: 1, 1, 1, 1>,\n'
                f'          padding = dense<0> : tensor<4x2xi64>\n'
                f'        }} : ({tensor_type(xshape, "i8")}, tensor<i8>) -> '
                f'{tensor_type((n, oh, ow, c), "i8")}')
            env[node.output[0]] = (y, (n, oh, ow, c))
            dtype[node.output[0]] = "i8"
            scale[node.output[0]] = s_in

        elif op == "Relu":
            # Symmetric quantization: real 0 maps to quantized 0, so on int8 a
            # ReLU is a plain integer max against zero. After the last Gemm the
            # tensor is float, so the same op runs in f32 instead.
            dt = dtype[node.input[0]]
            zero = e.splat(0, xshape, dt)
            y = e.op(f'"stablehlo.maximum"({x}, {zero}) : '
                     f'({tensor_type(xshape, dt)}, {tensor_type(xshape, dt)}) -> '
                     f'{tensor_type(xshape, dt)}')
            env[node.output[0]] = (y, xshape)
            dtype[node.output[0]] = dt
            scale[node.output[0]] = s_in

        elif op == "Reshape":
            n, h, w, c = xshape
            # The FC weights are tied to ONNX's C,H,W flatten order.
            t = e.op(f'"stablehlo.transpose"({x}) {{permutation = array<i64: 0, 3, 1, 2>}} '
                     f': ({tensor_type(xshape, "i8")}) -> {tensor_type((n, c, h, w), "i8")}')
            y = e.op(f'"stablehlo.reshape"({t}) : '
                     f'({tensor_type((n, c, h, w), "i8")}) -> '
                     f'{tensor_type((1, c * h * w), "i8")}')
            env[node.output[0]] = (y, (1, c * h * w))
            dtype[node.output[0]] = "i8"
            scale[node.output[0]] = s_in

        elif op == "Gemm":
            n, k = xshape
            wf = inits[node.input[1]].astype(F32)      # [out, in]
            bf = inits[node.input[2]].astype(F32)
            s_w = choose_scale(np.abs(wf).max())

            wq = np.clip(np.round(np.transpose(wf) / s_w), -QMAX, QMAX).astype(np.int8)
            wv = e.const_i8(wq)
            acc = e.op(
                f'"stablehlo.dot_general"({x}, {wv}) {{dot_dimension_numbers = {DDN}}} '
                f': ({tensor_type(xshape, "i8")}, '
                f'{tensor_type((k, wf.shape[0]), "i8")}) -> '
                f'{tensor_type((n, wf.shape[0]), "i32")}')

            is_last = node.output[0] in LAST_FLOAT_TENSORS
            if is_last:
                # Last layer: dequantize the accumulator to float so the output
                # is directly comparable with the ONNX Runtime reference.
                y = dequantize(e, acc, (n, wf.shape[0]), bf,
                               float(s_in) * float(s_w))
                env[node.output[0]] = (y, (n, wf.shape[0]))
                dtype[node.output[0]] = "f32"
                scale[node.output[0]] = s_in   # not used downstream, keeps lookups valid
                meta[node.output[0]] = dict(kind="fc_last", s_in=float(s_in),
                                            s_w=float(s_w), wq=wq,
                                            mnk=(n, wf.shape[0], k), bias_f=bf,
                                            scale=float(s_in) * float(s_w))
            else:
                s_out = choose_scale(calib[node.output[0]])
                M, shift = choose_multiplier(float(s_in) * float(s_w) / float(s_out))
                bias_i32 = np.round(bf / (float(s_in) * float(s_w))).astype(np.int32)
                y = requantize(e, acc, (n, wf.shape[0]), bias_i32, M, shift)
                env[node.output[0]] = (y, (n, wf.shape[0]))
                dtype[node.output[0]] = "i8"
                scale[node.output[0]] = s_out
                meta[node.output[0]] = dict(kind="fc", M=M, shift=shift,
                                            mnk=(n, wf.shape[0], k),
                                            s_in=float(s_in), s_w=float(s_w),
                                            s_out=float(s_out), bias=bias_i32,
                                            wq=wq, shape=(n, wf.shape[0]))

        elif op == "LogSoftmax":
            ex = e.op(f'"stablehlo.exponential"({x}) : '
                      f'({tensor_type(xshape)}) -> {tensor_type(xshape)}')
            zero = e.const_f32(np.zeros((), dtype=F32))
            s = e.op(
                f'"stablehlo.reduce"({ex}, {zero}) ({{\n'
                f'        ^bb0(%a: tensor<f32>, %b: tensor<f32>):\n'
                f'          %s = stablehlo.add %a, %b : tensor<f32>\n'
                f'          stablehlo.return %s : tensor<f32>\n'
                f'        }}) {{dimensions = array<i64: 1>}}\n'
                f'        : ({tensor_type(xshape)}, tensor<f32>) -> {tensor_type((xshape[0],))}')
            lg = e.op(f'"stablehlo.log"({s}) : ({tensor_type((xshape[0],))}) -> '
                      f'{tensor_type((xshape[0],))}')
            lgb = e.op(f'"stablehlo.broadcast_in_dim"({lg}) '
                       f'{{broadcast_dimensions = array<i64: 0>}} '
                       f': ({tensor_type((xshape[0],))}) -> {tensor_type(xshape)}')
            y = e.op(f'"stablehlo.subtract"({x}, {lgb}) : '
                     f'({tensor_type(xshape)}, {tensor_type(xshape)}) -> {tensor_type(xshape)}')
            env[node.output[0]] = (y, xshape)
            dtype[node.output[0]] = "f32"

        else:
            raise SystemExit(f"unsupported ONNX op: {op}")

    out = env[graph.output[0].name][0]
    e.lines.append(f"    return {out} : tensor<1x10xf32>")
    e.lines.append("  }")
    e.lines.append("}")
    return "\n".join(e.lines) + "\n", meta, scale


def requantize(e, acc, shape, bias_i32, M, shift):
    """acc_i32 + bias, scaled by M/2^shift, clamped and narrowed to i8."""
    bb = e.row_broadcast(bias_i32, shape, "i32")
    a = e.op(f'"stablehlo.add"({acc}, {bb}) : '
             f'({tensor_type(shape, "i32")}, {tensor_type(shape, "i32")}) -> '
             f'{tensor_type(shape, "i32")}')
    wide = e.cast(a, shape, "i32", "i64")
    prod = e.op(f'"stablehlo.multiply"({wide}, {e.splat(M, shape, "i64")}) : '
                f'({tensor_type(shape, "i64")}, {tensor_type(shape, "i64")}) -> '
                f'{tensor_type(shape, "i64")}')
    rounded = e.op(f'"stablehlo.add"({prod}, {e.splat(1 << (shift - 1), shape, "i64")}) : '
                   f'({tensor_type(shape, "i64")}, {tensor_type(shape, "i64")}) -> '
                   f'{tensor_type(shape, "i64")}')
    shifted = e.op(f'"stablehlo.shift_right_arithmetic"({rounded}, '
                   f'{e.splat(shift, shape, "i64")}) : '
                   f'({tensor_type(shape, "i64")}, {tensor_type(shape, "i64")}) -> '
                   f'{tensor_type(shape, "i64")}')
    narrow = e.cast(shifted, shape, "i64", "i32")
    lo = e.op(f'"stablehlo.maximum"({narrow}, {e.splat(-QMAX, shape, "i32")}) : '
              f'({tensor_type(shape, "i32")}, {tensor_type(shape, "i32")}) -> '
              f'{tensor_type(shape, "i32")}')
    hi = e.op(f'"stablehlo.minimum"({lo}, {e.splat(QMAX, shape, "i32")}) : '
              f'({tensor_type(shape, "i32")}, {tensor_type(shape, "i32")}) -> '
              f'{tensor_type(shape, "i32")}')
    return e.cast(hi, shape, "i32", "i8")


def dequantize(e, acc, shape, bias_f32, real_scale):
    # acc is in units of (s_in * s_w); the bias is already in real units, so it
    # is added *after* scaling, not before.
    f = e.cast(acc, shape, "i32", "f32")
    scaled = e.op(f'"stablehlo.multiply"({f}, {e.splat(real_scale, shape, "f32")}) : '
                  f'({tensor_type(shape)}, {tensor_type(shape)}) -> {tensor_type(shape)}')
    bb = e.row_broadcast(bias_f32, shape, "f32")
    return e.op(f'"stablehlo.add"({scaled}, {bb}) : '
                f'({tensor_type(shape)}, {tensor_type(shape)}) -> {tensor_type(shape)}')


# ---------------------------------------------------------------------------
# Calibration and the numpy reference
# ---------------------------------------------------------------------------

def make_calib(path, x_nchw):
    """Run the float model once and record max|x| for every tensor."""
    import onnxruntime as ort
    from onnx import TensorProto, helper

    mo = onnx.load(path)
    g = mo.graph
    existing = set(v.name for v in g.output)
    for n in g.node:
        for o in n.output:
            if o not in existing:
                g.output.append(helper.make_tensor_value_info(o, TensorProto.FLOAT, None))
                existing.add(o)
    scratch = pathlib.Path(__file__).resolve().parents[3] / "scratch"
    scratch.mkdir(parents=True, exist_ok=True)
    calib_path = str(scratch / "cnn_calib.onnx")
    onnx.save(mo, calib_path)

    so = ort.SessionOptions()
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
    sess = ort.InferenceSession(calib_path, so,
                                providers=["CPUExecutionProvider"])
    names = [o for n in onnx.load(path).graph.node for o in n.output]
    outs = sess.run(names, {sess.get_inputs()[0].name: x_nchw})
    calib = {n: float(np.abs(v).max()) for n, v in zip(names, outs)}
    calib[onnx.load(path).graph.input[0].name] = float(np.abs(x_nchw).max())
    return calib


def reference_int8(model, calib, x_nhwc):
    """A straightforward numpy implementation of the same quantized graph.

    Same scales, same integer requantize, same layout. If IREE agrees with
    this bit for bit, the graph means what we think it means.
    """
    graph = model.graph
    inits = {i.name: numpy_helper.to_array(i) for i in graph.initializer}
    for node in graph.node:
        if node.op_type == "Constant":
            for attr in node.attribute:
                if attr.name == "value":
                    inits[node.output[0]] = numpy_helper.to_array(attr.t)

    s_in0 = choose_scale(calib[graph.input[0].name])
    q = lambda x, s: np.clip(np.round(x / s), -QMAX, QMAX).astype(np.int8)

    env = {graph.input[0].name: q(x_nhwc, s_in0)}
    scale = {graph.input[0].name: s_in0}

    producer = {o: n for n in graph.node for o in n.output}
    LAST_FLOAT_TENSORS = set()
    for n in graph.node:
        if n.op_type == "LogSoftmax":
            t = n.input[0]
            while t in producer and producer[t].op_type in ("Relu", "Reshape"):
                t = producer[t].input[0]
            LAST_FLOAT_TENSORS.add(t)

    import os
    DBG = os.environ.get("DQ")
    for node in graph.node:
        op = node.op_type
        attrs = {a.name: onnx.helper.get_attribute_value(a) for a in node.attribute}
        if op == "Constant":
            continue
        if DBG:
            v = env[node.input[0]]
            print(f"  in  {node.input[0]:>4} {op:<10} shape={str(v.shape):<16} "
                  f"min={v.min():9.4f} max={v.max():9.4f} s={float(scale[node.input[0]]):.6f}")
        x = env[node.input[0]].astype(np.int32)
        s_in = scale[node.input[0]]

        if op == "Conv":
            wf = inits[node.input[1]]
            bf = inits[node.input[2]].astype(F32)
            kh, kw = wf.shape[2], wf.shape[3]
            n, h, w, c = env[node.input[0]].shape
            oh, ow = h - kh + 1, w - kw + 1
            s_out = choose_scale(calib[node.output[0]])
            s_w = choose_scale(np.abs(wf).max())
            wt = np.transpose(wf, (2, 3, 1, 0)).reshape(kh * kw * c, wf.shape[0])
            wq = q(wt, s_w).astype(np.int32)
            patches = np.zeros((oh * ow, kh * kw * c), dtype=np.int32)
            xi = env[node.input[0]].astype(np.int32)
            for i in range(kh):
                for j in range(kw):
                    col = i * kw * c + j * c
                    patches[:, col:col + c] = xi[0, i:i + oh, j:j + ow, :].reshape(-1, c)
            acc = patches @ wq
            M, shift = choose_multiplier(float(s_in) * float(s_w) / float(s_out))
            acc = acc + np.round(bf / (float(s_in) * float(s_w))).astype(np.int32)
            y = requant_np(acc, M, shift).reshape(1, oh, ow, wf.shape[0])
            env[node.output[0]] = y
            scale[node.output[0]] = s_out

        elif op == "MaxPool":
            kh, kw = attrs["kernel_shape"]
            n, h, w, c = env[node.input[0]].shape
            oh, ow = (h - kh) // kh + 1, (w - kw) // kw + 1
            xi = env[node.input[0]]
            out = np.full((n, oh, ow, c), -128, dtype=np.int8)
            for i in range(kh):
                for j in range(kw):
                    out = np.maximum(out, xi[:, i:i + oh * kh:kh, j:j + ow * kw:kw, :])
            env[node.output[0]] = out
            scale[node.output[0]] = s_in

        elif op == "Relu":
            # Keep the dtype: after the last Gemm this tensor is float, and
            # casting it back to int8 would truncate 0.74 to 0.
            v = env[node.input[0]]
            env[node.output[0]] = np.maximum(v, 0).astype(v.dtype)
            scale[node.output[0]] = s_in

        elif op == "Reshape":
            xi = env[node.input[0]]
            n, h, w, c = xi.shape
            env[node.output[0]] = np.transpose(xi, (0, 3, 1, 2)).reshape(1, c * h * w)
            scale[node.output[0]] = s_in

        elif op == "Gemm":
            wf = inits[node.input[1]].astype(F32)
            bf = inits[node.input[2]].astype(F32)
            s_w = choose_scale(np.abs(wf).max())
            wq = q(np.transpose(wf), s_w).astype(np.int32)
            acc = env[node.input[0]].astype(np.int32) @ wq
            is_last = node.output[0] in LAST_FLOAT_TENSORS
            if is_last:
                y = acc * np.float32(float(s_in) * float(s_w)) + bf.reshape(1, -1)
                env[node.output[0]] = y.astype(F32)
                scale[node.output[0]] = s_in   # unused downstream
            else:
                s_out = choose_scale(calib[node.output[0]])
                M, shift = choose_multiplier(float(s_in) * float(s_w) / float(s_out))
                acc = acc + np.round(bf / (float(s_in) * float(s_w))).astype(np.int32)
                env[node.output[0]] = requant_np(acc, M, shift)
                scale[node.output[0]] = s_out

        elif op == "LogSoftmax":
            xi = env[node.input[0]].astype(np.float64)
            m = xi.max(axis=1, keepdims=True)
            env[node.output[0]] = (xi - m - np.log(np.exp(xi - m).sum(axis=1, keepdims=True))
                                   ).astype(F32)

    return env[graph.output[0].name]


def write_header(path, x_nhwc, expected):
    qin = np.clip(np.round(x_nhwc / choose_scale(np.abs(x_nhwc).max())),
                  -QMAX, QMAX).astype(np.int8)
    flat_in = qin.reshape(-1)
    flat_out = np.asarray(expected, dtype=F32).reshape(-1)
    with open(path, "w") as f:
        f.write("// Generated by tools/emit_int8.py -- do not edit.\n")
        f.write("// Reference = the numpy int8 implementation of the same graph.\n")
        f.write("#include <stddef.h>\n#include <stdint.h>\n\n")
        f.write(f"static const size_t cnn_input_count = {flat_in.size};\n")
        f.write("static const int8_t cnn_input[%d] = {\n" % flat_in.size)
        for i in range(0, flat_in.size, 16):
            f.write("  " + ", ".join(str(int(v)) for v in flat_in[i:i + 16]) + ",\n")
        f.write("};\n\n")
        f.write(f"static const size_t cnn_output_count = {flat_out.size};\n")
        f.write("static const float cnn_expected[%d] = {\n" % flat_out.size)
        for v in flat_out:
            f.write(f"  {float(v)!r}f,\n")
        f.write("};\n")


GGEM_HEADER = """
// Generated by tools/emit_int8.py -- do not edit.
//
// Everything a hand-written kernel for this dispatch needs: the quantized
// weights, the folded bias, and the fixed-point requantize parameters.
// IREE fused matmul + bias + requantize into one dispatch, so an interposed
// kernel has to reproduce all three, not just the matmul.
"""


def write_gemmini_params(path, meta):
    with open(path, "w") as f:
        f.write(GGEM_HEADER)
        f.write("#include <stdint.h>\n\n")
        for name, m in meta.items():
            if m["kind"] not in ("conv", "fc", "fc_last"):
                continue
            M_, N_, K_ = m["mnk"]
            tag = f"{M_}_{N_}_{K_}"
            f.write(f"// layer '{name}': {M_}x{N_}x{K_}\n")
            wq = np.asarray(m["wq"]).reshape(-1)
            f.write(f"static const int8_t gemm_w_{tag}[{wq.size}] = {{\n")
            for i in range(0, wq.size, 16):
                f.write("  " + ", ".join(str(int(v)) for v in wq[i:i + 16]) + ",\n")
            f.write("};\n")
            if m["kind"] == "fc_last":
                # Final layer stays in float: dequantize the accumulator and
                # add the real bias, so the values are comparable with ONNX.
                b = np.asarray(m["bias_f"]).reshape(-1).astype(np.float32)
                f.write(f"static const float gemm_biasf_{tag}[{b.size}] = {{\n  "
                        + ", ".join(float(v).hex() + "f" for v in b) + "};\n")
                f.write(f"static const float gemm_scale_{tag} = "
                        f"{float(m['scale']).hex()}f;\n\n")
            else:
                b = np.asarray(m["bias"]).reshape(-1)
                f.write(f"static const int32_t gemm_bias_{tag}[{b.size}] = {{\n  "
                        + ", ".join(str(int(v)) for v in b) + "};\n")
                f.write(f"static const int32_t gemm_mult_{tag} = {int(m['M'])};\n")
                f.write(f"static const int gemm_shift_{tag} = {int(m['shift'])};\n\n")
        f.write("""// Applies the fused epilogue IREE generated:
//   v = acc + bias; p = (int64)v * mult + (1 << (shift-1));
//   v = p >> shift; clamp to [-127, 127]
static inline int8_t gemm_requant(int32_t acc, int32_t bias, int32_t mult,
                                  int shift) {
  int64_t p = ((int64_t)(acc + bias)) * mult + ((int64_t)1 << (shift - 1));
  p >>= shift;
  if (p < -127) p = -127;
  if (p > 127) p = 127;
  return (int8_t)p;
}
""")


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    src, out_mlir, out_hdr = argv[1], argv[2], argv[3]

    model = onnx.load(src)
    x_nchw = make_input()
    x_nhwc = np.transpose(x_nchw, (0, 2, 3, 1))
    calib = make_calib(src, x_nchw)

    mlir, meta, _ = emit_int8(model, calib)
    with open(out_mlir, "w") as f:
        f.write(mlir)
    print(f"wrote {out_mlir} ({len(mlir)} bytes)")

    params = out_mlir.rsplit("/", 1)[0] + "/gemmini_params.h"
    write_gemmini_params(params, meta)
    print(f"wrote {params}")

    ref = reference_int8(model, calib, x_nhwc)
    write_header(out_hdr, x_nhwc, ref)
    print(f"wrote {out_hdr}")
    print("numpy int8 reference:", np.array2string(ref.reshape(-1), precision=5))
    print("argmax:", int(np.argmax(ref.reshape(-1))))
    for name, m in meta.items():
        if m["kind"] in ("conv", "fc"):
            print(f"  {name}: M={m['M']} shift={m['shift']} "
                  f"s_in={m['s_in']:.6f} s_w={m['s_w']:.6f} s_out={m['s_out']:.6f}")
        else:
            print(f"  {name}: final layer, dequantized to float")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
