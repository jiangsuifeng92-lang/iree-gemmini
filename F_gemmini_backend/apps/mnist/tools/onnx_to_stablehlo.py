#!/usr/bin/env python3
"""Turn the small MNIST CNN from ONNX into StableHLO MLIR.

Why this exists
---------------
IREE has no ONNX front end. Its inputs are StableHLO, TOSA and Torch; ONNX goes
to ONNX Runtime, not here. So to get a *real* CNN -- one with convolutions and
real weights, not a hand-written matmul -- through IREE, something has to
translate the graph.

This is that translator, for one graph. It is not a general ONNX importer: it
handles exactly the ops in model/cnn_mnist.onnx (Conv, MaxPool, Relu, Reshape,
Gemm, LogSoftmax) and refuses anything else rather than guessing.

The useful property is that it also produces the reference I/O: the same input
is run through ONNX Runtime, and both the input and ORT's output are emitted as
C arrays. The bare-metal program then compares IREE's answer against ONNX
Runtime's answer, on the same numbers -- which is a far stronger check than
"it ran without crashing".

Layout notes
------------
ONNX is NCHW; StableHLO's convolution is written here in NHWC. Every layout
change is done in numpy on constants at export time, except one explicit
`stablehlo.transpose` before the flatten, because the FC weights are tied to
the NCHW flatten order.

Usage:
    python3 tools/onnx_to_stablehlo.py \
        model/cnn_mnist.onnx gen/cnn.mlir gen/cnn_io.h
"""

import sys

import numpy as np
import onnx
from onnx import numpy_helper

F32 = np.float32


# ---------------------------------------------------------------------------
# MLIR text helpers
# ---------------------------------------------------------------------------

def _fmt(v):
    """Format one float so that round-tripping through text is exact."""
    a = float(v)
    if a != a:
        return "0x7FC00000"      # NaN
    if a == float("inf"):
        return "0x7F800000"
    if a == float("-inf"):
        return "0xFF800000"
    return repr(a)


def dense(arr):
    """Serialize a numpy array as a nested `dense<...>` literal.

    Nested rather than flat on purpose: IREE's stablehlo parser rejects flat
    literals for rank > 1.
    """
    a = np.asarray(arr)
    if a.ndim == 0:
        return _fmt(a)
    if a.ndim == 1:
        return "[" + ", ".join(_fmt(x) for x in a) + "]"
    return "[" + ", ".join(dense(sub) for sub in a) + "]"


def tensor_type(shape):
    if len(shape) == 0:
        return "tensor<f32>"     # rank-0 is `tensor<f32>`, not `tensor<xf32>`
    return "tensor<" + "x".join(str(d) for d in shape) + "xf32>"


class Emitter:
    def __init__(self):
        self.lines = []
        self.n = 0

    def val(self):
        self.n += 1
        return f"%{self.n}"

    def const(self, arr):
        v = self.val()
        a = np.asarray(arr, dtype=F32)
        self.lines.append(
            f"    {v} = stablehlo.constant dense<{dense(a)}> : {tensor_type(a.shape)}")
        return v

    def op(self, text):
        v = self.val()
        self.lines.append(f"    {v} = {text}")
        return v

    def broadcast(self, value, from_shape, to_shape):
        """Broadcast a rank-N value up to a larger rank-N shape.

        StableHLO's element-wise ops require identical operand shapes -- there
        is no implicit broadcasting like NumPy or ONNX. Every bias add and
        every scalar compare therefore needs an explicit broadcast_in_dim.
        """
        if tuple(from_shape) == tuple(to_shape):
            return value
        dims = list(range(len(from_shape)))
        return self.op(
            f'"stablehlo.broadcast_in_dim"({value}) '
            f'{{broadcast_dimensions = array<i64: {", ".join(map(str, dims))}>}} '
            f': ({tensor_type(from_shape)}) -> {tensor_type(to_shape)}')


# ---------------------------------------------------------------------------
# ONNX -> StableHLO
# ---------------------------------------------------------------------------

def nchw_to_nhwc_w(w):
    """ONNX conv weight [K, C, kh, kw] -> StableHLO NHWC kernel [kh, kw, C, K]."""
    return np.transpose(w, (2, 3, 1, 0)).astype(F32)


def emit_graph(model):
    graph = model.graph
    inits = {i.name: numpy_helper.to_array(i) for i in graph.initializer}

    # The reshape target lives in a Constant node, not an initializer.
    for node in graph.node:
        if node.op_type == "Constant":
            for attr in node.attribute:
                if attr.name == "value":
                    inits[node.output[0]] = numpy_helper.to_array(attr.t)

    e = Emitter()
    shape = [d.dim_value for d in graph.input[0].type.tensor_type.shape.dim]
    e.lines.append("module {")
    e.lines.append("  func.func @main(%arg0: tensor<1x28x28x1xf32>) -> tensor<1x10xf32> {")

    env = {}          # onnx tensor name -> (mlir value, logical shape)

    # ONNX input is NCHW [1,1,28,28]; the host feeds NHWC [1,28,28,1] so that
    # the convolutions below need no transposes. Same data, different strides.
    env[graph.input[0].name] = ("%arg0", (1, 28, 28, 1))

    for node in graph.node:
        op = node.op_type
        attrs = {a.name: onnx.helper.get_attribute_value(a) for a in node.attribute}
        ins = [env.get(x) for x in node.input if x and x in env]

        if op == "Constant":
            continue

        elif op == "Conv":
            x, xshape = ins[0]
            w = nchw_to_nhwc_w(inits[node.input[1]])
            b = inits[node.input[2]].astype(F32)
            kh, kw = w.shape[0], w.shape[1]
            assert attrs.get("pads", [0, 0, 0, 0]) == [0, 0, 0, 0], "padding not handled"
            assert attrs.get("group", 1) == 1, "groups not handled"
            sh, sw = attrs.get("strides", [1, 1])
            dh, dw = attrs.get("dilations", [1, 1])
            assert (dh, dw) == (1, 1), "dilation not handled"

            n, h, wd, c = xshape
            oh = (h - kh) // sh + 1
            ow = (wd - kw) // sw + 1
            wv = e.const(w)
            # Quoted generic form: the bare `stablehlo.convolution(a, b)` spelling
            # is the pretty syntax, which expects a compact `dim_numbers`
            # argument instead of the attribute below.
            y = e.op(
                f'"stablehlo.convolution"({x}, {wv}) {{\n'
                f'          batch_group_count = 1 : i64,\n'
                f'          dimension_numbers = #stablehlo.conv<raw\n'
                f'            input_batch_dimension = 0,\n'
                f'            input_feature_dimension = 3,\n'
                f'            input_spatial_dimensions = [1, 2],\n'
                f'            kernel_input_feature_dimension = 2,\n'
                f'            kernel_output_feature_dimension = 3,\n'
                f'            kernel_spatial_dimensions = [0, 1],\n'
                f'            output_batch_dimension = 0,\n'
                f'            output_feature_dimension = 3,\n'
                f'            output_spatial_dimensions = [1, 2]\n'
                f'        >,\n'
                f'          feature_group_count = 1 : i64,\n'
                f'          rhs_dilation = array<i64: {dh}, {dw}>,\n'
                f'          window_strides = array<i64: {sh}, {sw}>}}\n'
                f'        : ({tensor_type(xshape)}, {tensor_type(w.shape)}) -> '
                f'{tensor_type((n, oh, ow, w.shape[3]))}')
            bv = e.const(b.reshape(1, 1, 1, -1))
            bshape = (1, 1, 1, w.shape[3])
            out_shape = (n, oh, ow, w.shape[3])
            bb = e.broadcast(bv, bshape, out_shape)
            y = e.op(f'"stablehlo.add"({y}, {bb}) : '
                     f'({tensor_type(out_shape)}, {tensor_type(out_shape)}) -> {tensor_type(out_shape)}')
            env[node.output[0]] = (y, out_shape)

        elif op == "MaxPool":
            x, xshape = ins[0]
            n, h, wd, c = xshape
            kh, kw = attrs["kernel_shape"]
            sh, sw = attrs.get("strides", [1, 1])
            assert attrs.get("pads", [0, 0, 0, 0]) == [0, 0, 0, 0], "padding not handled"
            oh, ow = (h - kh) // sh + 1, (wd - kw) // sw + 1
            neg_inf = e.const(np.array(-np.inf, dtype=F32))
            y = e.op(
                f'"stablehlo.reduce_window"({x}, {neg_inf}) ({{\n'
                f'        ^bb0(%a: tensor<f32>, %b: tensor<f32>):\n'
                f'          %m = stablehlo.maximum %a, %b : tensor<f32>\n'
                f'          stablehlo.return %m : tensor<f32>\n'
                f'        }}) {{\n'
                f'          window_dimensions = array<i64: 1, {kh}, {kw}, 1>,\n'
                f'          window_strides = array<i64: 1, {sh}, {sw}, 1>,\n'
                f'          base_dilations = array<i64: 1, 1, 1, 1>,\n'
                f'          window_dilations = array<i64: 1, 1, 1, 1>,\n'
                f'          padding = dense<0> : tensor<4x2xi64>\n'
                f'        }} : ({tensor_type(xshape)}, tensor<f32>) -> '
                f'{tensor_type((n, oh, ow, c))}')
            env[node.output[0]] = (y, (n, oh, ow, c))

        elif op == "Relu":
            x, xshape = ins[0]
            zero = e.const(np.zeros((1,) * len(xshape), dtype=F32))
            zb = e.broadcast(zero, (1,) * len(xshape), xshape)
            y = e.op(f'"stablehlo.maximum"({x}, {zb}) : '
                     f'({tensor_type(xshape)}, {tensor_type(xshape)}) -> {tensor_type(xshape)}')
            env[node.output[0]] = (y, xshape)

        elif op == "Reshape":
            x, xshape = ins[0]
            target = [int(v) for v in inits[node.input[1]].tolist()]
            n, h, wd, c = xshape
            if target[-1] == c * h * wd:
                # ONNX flattens NCHW ([1, C, H, W]) to [1, C*H*W]. Our tensor is
                # NHWC, where the same elements in the same order are
                # [C, H, W] -- so one transpose first, otherwise the FC weights
                # would be applied to a permuted vector.
                x = e.op(f'"stablehlo.transpose"({x}) {{permutation = array<i64: 0, 3, 1, 2>}} '
                         f': ({tensor_type(xshape)}) -> {tensor_type((n, c, h, wd))}')
                xshape = (n, c, h, wd)
                flat = (1, c * h * wd)
            else:
                raise SystemExit(f"unhandled reshape target {target} for shape {xshape}")
            y = e.op(f'"stablehlo.reshape"({x}) : ({tensor_type(xshape)}) -> {tensor_type(flat)}')
            env[node.output[0]] = (y, flat)

        elif op == "Gemm":
            # y = alpha * A * B + beta * C, with transB=1, so B is [out, in].
            x, xshape = ins[0]
            w = inits[node.input[1]].astype(F32)   # [out, in]
            bt = np.transpose(w).astype(F32)       # [in, out]
            assert attrs.get("alpha", 1.0) == 1.0 and attrs.get("beta", 1.0) == 1.0
            m, k = xshape
            wv = e.const(bt)
            ddn = ('#stablehlo.dot<lhs_batching_dimensions = [], '
                   'rhs_batching_dimensions = [], '
                   'lhs_contracting_dimensions = [1], '
                   'rhs_contracting_dimensions = [0]>')
            y = e.op(f'"stablehlo.dot_general"({x}, {wv}) '
                     f'{{dot_dimension_numbers = {ddn}}} '
                     f': ({tensor_type(xshape)}, {tensor_type(bt.shape)}) -> '
                     f'{tensor_type((m, bt.shape[1]))}')
            if len(node.input) > 2 and node.input[2] in inits:
                bv = e.const(inits[node.input[2]].astype(F32).reshape(1, -1))
                y = e.op(f'"stablehlo.add"({y}, {bv}) : '
                         f'({tensor_type((m, bt.shape[1]))}, {tensor_type((m, bt.shape[1]))}) -> '
                         f'{tensor_type((m, bt.shape[1]))}')
            env[node.output[0]] = (y, (m, bt.shape[1]))

        elif op == "LogSoftmax":
            x, xshape = ins[0]
            ex = e.op(f'"stablehlo.exponential"({x}) : ({tensor_type(xshape)}) -> {tensor_type(xshape)}')
            zero = e.const(np.zeros((), dtype=F32))
            s = e.op(
                f'"stablehlo.reduce"({ex}, {zero}) ({{\n'
                f'        ^bb0(%a: tensor<f32>, %b: tensor<f32>):\n'
                f'          %s = stablehlo.add %a, %b : tensor<f32>\n'
                f'          stablehlo.return %s : tensor<f32>\n'
                f'        }}) {{dimensions = array<i64: 1>}}\n'
                f'        : ({tensor_type(xshape)}, tensor<f32>) -> {tensor_type((xshape[0],))}')
            lg = e.op(f'"stablehlo.log"({s}) : ({tensor_type((xshape[0],))}) -> {tensor_type((xshape[0],))}')
            lgb = e.op(f'"stablehlo.broadcast_in_dim"({lg}) '
                       f'{{broadcast_dimensions = array<i64: 0>}} '
                       f': ({tensor_type((xshape[0],))}) -> {tensor_type(xshape)}')
            y = e.op(f'"stablehlo.subtract"({x}, {lgb}) : '
                     f'({tensor_type(xshape)}, {tensor_type(xshape)}) -> {tensor_type(xshape)}')
            env[node.output[0]] = (y, xshape)

        else:
            raise SystemExit(f"unsupported ONNX op: {op}")

    out = env[graph.output[0].name][0]
    e.lines.append(f"    return {out} : {tensor_type((1, 10))}")
    e.lines.append("  }")
    e.lines.append("}")
    return "\n".join(e.lines) + "\n", shape


# ---------------------------------------------------------------------------
# Reference I/O
# ---------------------------------------------------------------------------

def make_input():
    """A deterministic 28x28 pattern.

    Not a real MNIST digit -- the point of this step is that IREE and ONNX
    Runtime agree on the same numbers, not that the classifier is accurate.
    A fixed pattern keeps the test reproducible with no data files.
    """
    y, x = np.mgrid[0:28, 0:28]
    img = (np.sin(x * 0.55) * np.cos(y * 0.4) * 0.5 +
           ((x - 14) ** 2 + (y - 14) ** 2 < 40) * 0.8)
    return img.astype(F32).reshape(1, 1, 28, 28)


def run_onnxruntime(path, x_nchw):
    import onnxruntime as ort
    sess = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    return sess.run(None, {sess.get_inputs()[0].name: x_nchw})[0]


def write_header(path, x_nhwc, expected):
    flat_in = x_nhwc.reshape(-1)
    flat_out = expected.reshape(-1)
    with open(path, "w") as f:
        f.write("// Generated by tools/onnx_to_stablehlo.py -- do not edit.\n")
        f.write("// Reference values produced by ONNX Runtime on the same input.\n")
        f.write("#include <stddef.h>\n\n")
        f.write(f"static const size_t cnn_input_count = {flat_in.size};\n")
        f.write("static const float cnn_input[%d] = {\n" % flat_in.size)
        for i in range(0, flat_in.size, 8):
            f.write("  " + ", ".join(f"{v!r}f" for v in flat_in[i:i + 8]) + ",\n")
        f.write("};\n\n")
        f.write(f"static const size_t cnn_output_count = {flat_out.size};\n")
        f.write("static const float cnn_expected[%d] = {\n" % flat_out.size)
        for v in flat_out:
            f.write(f"  {v!r}f,\n")
        f.write("};\n")


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    src, out_mlir, out_hdr = argv[1], argv[2], argv[3]

    model = onnx.load(src)
    mlir, _ = emit_graph(model)
    with open(out_mlir, "w") as f:
        f.write(mlir)
    print(f"wrote {out_mlir} ({len(mlir)} bytes)")

    x_nchw = make_input()
    expected = run_onnxruntime(src, x_nchw)
    x_nhwc = np.transpose(x_nchw, (0, 2, 3, 1))
    write_header(out_hdr, x_nhwc, expected)
    print(f"wrote {out_hdr}")
    print("ONNX Runtime output:", np.array2string(expected.reshape(-1), precision=5))
    print("argmax:", int(np.argmax(expected.reshape(-1))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
