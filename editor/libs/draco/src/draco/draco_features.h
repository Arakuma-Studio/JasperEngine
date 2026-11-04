// JasperEngine: what Draco's CMake generates as draco/draco_features.h for its default C++
// build (cmake/draco_options.cmake, draco_set_optional_features), written by hand since only the
// decoder is vendored (README.jasper.md). Every bitstream an encoder writes decodes.
#ifndef DRACO_FEATURES_H_
#define DRACO_FEATURES_H_

#define DRACO_POINT_CLOUD_COMPRESSION_SUPPORTED
#define DRACO_MESH_COMPRESSION_SUPPORTED
#define DRACO_NORMAL_ENCODING_SUPPORTED
#define DRACO_STANDARD_EDGEBREAKER_SUPPORTED
#define DRACO_PREDICTIVE_EDGEBREAKER_SUPPORTED
#define DRACO_BACKWARDS_COMPATIBILITY_SUPPORTED
#define DRACO_ATTRIBUTE_INDICES_DEDUPLICATION_SUPPORTED
#define DRACO_ATTRIBUTE_VALUES_DEDUPLICATION_SUPPORTED

#endif  // DRACO_FEATURES_H_
