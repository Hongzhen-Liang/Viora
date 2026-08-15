"""DS-CNN architecture kept within TFLite Micro's integer operator set."""

from __future__ import annotations

import tensorflow as tf
from tensorflow.keras import Model, layers

from hi_vesper_config import FEATURE_SHAPE


def depthwise_separable_block(x: tf.Tensor, filters: int, stride: int, name: str) -> tf.Tensor:
    x = layers.DepthwiseConv2D(
        3,
        strides=(stride, stride),
        padding="same",
        use_bias=False,
        name=f"{name}_depthwise",
    )(x)
    x = layers.BatchNormalization(name=f"{name}_dw_bn")(x)
    x = layers.ReLU(name=f"{name}_dw_relu")(x)
    x = layers.Conv2D(filters, 1, use_bias=False, name=f"{name}_pointwise")(x)
    x = layers.BatchNormalization(name=f"{name}_pw_bn")(x)
    return layers.ReLU(name=f"{name}_pw_relu")(x)


def build_model() -> Model:
    inputs = layers.Input(shape=FEATURE_SHAPE, name="logmel")
    x = layers.Conv2D(
        16,
        3,
        strides=(2, 2),
        padding="same",
        use_bias=False,
        name="stem_conv",
    )(inputs)
    x = layers.BatchNormalization(name="stem_bn")(x)
    x = layers.ReLU(name="stem_relu")(x)
    x = depthwise_separable_block(x, 24, 1, "ds1")
    x = depthwise_separable_block(x, 32, 2, "ds2")
    x = depthwise_separable_block(x, 48, 1, "ds3")
    x = depthwise_separable_block(x, 64, 2, "ds4")
    x = depthwise_separable_block(x, 80, 1, "ds5")
    x = layers.GlobalAveragePooling2D(name="global_average")(x)
    x = layers.Dropout(0.15, name="dropout")(x)
    logits = layers.Dense(3, name="logits")(x)
    outputs = layers.Softmax(name="probabilities")(logits)
    return Model(inputs, outputs, name="hi_vesper_ds_cnn")
