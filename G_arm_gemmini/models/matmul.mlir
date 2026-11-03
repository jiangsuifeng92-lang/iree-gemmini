module {
  func.func @main(%a: tensor<64x64xi8>, %b: tensor<64x64xi8>) -> tensor<64x64xi32> {
    %c = "stablehlo.dot_general"(%a, %b) {
      dot_dimension_numbers = #stablehlo.dot<lhs_batching_dimensions = [],
        rhs_batching_dimensions = [], lhs_contracting_dimensions = [1],
        rhs_contracting_dimensions = [0]>
    } : (tensor<64x64xi8>, tensor<64x64xi8>) -> tensor<64x64xi32>
    return %c : tensor<64x64xi32>
  }
}
