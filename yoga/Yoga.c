/**
 * Copyright (c) 2014-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <string.h>

#include "YGNodeList.h"
#include "Yoga-internal.h"
#include "Yoga.h"

#ifdef _MSC_VER
#include <float.h>
#ifndef isnan
#define isnan _isnan
#endif

#ifndef __cplusplus
#define inline __inline
#endif

/* define fmaxf if < VC12 */
#if _MSC_VER < 1800
__forceinline const float fmaxf(const float a, const float b) {
  return (a > b) ? a : b;
}
#endif
#endif

typedef struct YGCachedMeasurement {
  float availableWidth;
  float availableHeight;
  YGMeasureMode widthMeasureMode;
  YGMeasureMode heightMeasureMode;

  float computedWidth;
  float computedHeight;
} YGCachedMeasurement;

// This value was chosen based on empiracle data. Even the most complicated
// layouts should not require more than 16 entries to fit within the cache.
#define YG_MAX_CACHED_RESULT_COUNT 16

typedef struct YGLayout {
  float position[4];
  float dimensions[2];
  float margin[6];
  float border[6];
  float padding[6];
  YGDirection direction;

  uint32_t computedFlexBasisGeneration;
  float computedFlexBasis;
  bool hadOverflow;

  // Instead of recomputing the entire layout every single time, we
  // cache some information to break early when nothing changed
  uint32_t generationCount;
  YGDirection lastParentDirection;

  uint32_t nextCachedMeasurementsIndex;
  YGCachedMeasurement cachedMeasurements[YG_MAX_CACHED_RESULT_COUNT];
  float measuredDimensions[2];

  YGCachedMeasurement cachedLayout;
} YGLayout;

typedef struct YGStyle {
  YGDirection direction;
  YGFlexDirection flexDirection;
  YGJustify justifyContent;
  YGAlign alignContent;
  YGAlign alignItems;
  YGAlign alignSelf;
  YGPositionType positionType;
  YGWrap flexWrap;
  YGOverflow overflow;
  YGDisplay display;
  float flex;
  float flexGrow;
  float flexShrink;
  YGValue flexBasis;
  YGValue margin[YGEdgeCount];
  YGValue position[YGEdgeCount];
  YGValue padding[YGEdgeCount];
  YGValue border[YGEdgeCount];
  YGValue dimensions[2];
  YGValue minDimensions[2];
  YGValue maxDimensions[2];

  // Yoga specific properties, not compatible with flexbox specification
  float aspectRatio;
} YGStyle;

typedef struct YGConfig {
  bool experimentalFeatures[YGExperimentalFeatureCount + 1];
  bool useWebDefaults;
  bool useLegacyStretchBehaviour;
  float pointScaleFactor;
  YGLogger logger;
  void *context;
} YGConfig;

typedef struct YGNode {
  YGStyle style;
  YGLayout layout;
  uint32_t lineIndex;

  YGNodeRef parent;
  YGNodeListRef children;

  struct YGNode *nextChild;

  YGMeasureFunc measure;
  YGBaselineFunc baseline;
  YGPrintFunc print;
  YGConfigRef config;
  void *context;

  bool isDirty;
  bool hasNewLayout;
  YGNodeType nodeType;

  YGValue const *resolvedDimensions[2];
} YGNode;

#define YG_UNDEFINED_VALUES \
  { .value = YGUndefined, .unit = YGUnitUndefined }

#define YG_AUTO_VALUES \
  { .value = YGUndefined, .unit = YGUnitAuto }

#define YG_DEFAULT_EDGE_VALUES_UNIT                                                   \
  {                                                                                   \
    [YGEdgeLeft] = YG_UNDEFINED_VALUES, [YGEdgeTop] = YG_UNDEFINED_VALUES,            \
    [YGEdgeRight] = YG_UNDEFINED_VALUES, [YGEdgeBottom] = YG_UNDEFINED_VALUES,        \
    [YGEdgeStart] = YG_UNDEFINED_VALUES, [YGEdgeEnd] = YG_UNDEFINED_VALUES,           \
    [YGEdgeHorizontal] = YG_UNDEFINED_VALUES, [YGEdgeVertical] = YG_UNDEFINED_VALUES, \
    [YGEdgeAll] = YG_UNDEFINED_VALUES,                                                \
  }

#define YG_DEFAULT_DIMENSION_VALUES \
  { [YGDimensionWidth] = YGUndefined, [YGDimensionHeight] = YGUndefined, }

#define YG_DEFAULT_DIMENSION_VALUES_UNIT \
  { [YGDimensionWidth] = YG_UNDEFINED_VALUES, [YGDimensionHeight] = YG_UNDEFINED_VALUES, }

#define YG_DEFAULT_DIMENSION_VALUES_AUTO_UNIT \
  { [YGDimensionWidth] = YG_AUTO_VALUES, [YGDimensionHeight] = YG_AUTO_VALUES, }

static const float kDefaultFlexGrow = 0.0f;
static const float kDefaultFlexShrink = 0.0f;
static const float kWebDefaultFlexShrink = 1.0f;

static const YGNode gYGNodeDefaults = {
    .parent = NULL,
    .children = NULL,
    .hasNewLayout = true,
    .isDirty = false,
    .nodeType = YGNodeTypeDefault,
    .resolvedDimensions = {[YGDimensionWidth] = &YGValueUndefined,
                           [YGDimensionHeight] = &YGValueUndefined},

    .style =
        {
            .flex = YGUndefined,
            .flexGrow = YGUndefined,
            .flexShrink = YGUndefined,
            .flexBasis = YG_AUTO_VALUES,
            .justifyContent = YGJustifyFlexStart,
            .alignItems = YGAlignStretch,
            .alignContent = YGAlignFlexStart,
            .direction = YGDirectionInherit,
            .flexDirection = YGFlexDirectionColumn,
            .overflow = YGOverflowVisible,
            .display = YGDisplayFlex,
            .dimensions = YG_DEFAULT_DIMENSION_VALUES_AUTO_UNIT,
            .minDimensions = YG_DEFAULT_DIMENSION_VALUES_UNIT,
            .maxDimensions = YG_DEFAULT_DIMENSION_VALUES_UNIT,
            .position = YG_DEFAULT_EDGE_VALUES_UNIT,
            .margin = YG_DEFAULT_EDGE_VALUES_UNIT,
            .padding = YG_DEFAULT_EDGE_VALUES_UNIT,
            .border = YG_DEFAULT_EDGE_VALUES_UNIT,
            .aspectRatio = YGUndefined,
        },

    .layout =
        {
            .dimensions = YG_DEFAULT_DIMENSION_VALUES,
            .lastParentDirection = (YGDirection) -1,
            .nextCachedMeasurementsIndex = 0,
            .computedFlexBasis = YGUndefined,
            .hadOverflow = false,
            .measuredDimensions = YG_DEFAULT_DIMENSION_VALUES,

            .cachedLayout =
                {
                    .widthMeasureMode = (YGMeasureMode) -1,
                    .heightMeasureMode = (YGMeasureMode) -1,
                    .computedWidth = -1,
                    .computedHeight = -1,
                },
        },
};

#ifdef ANDROID
static int YGAndroidLog(const YGConfigRef config,
                        const YGNodeRef node,
                        YGLogLevel level,
                        const char *format,
                        va_list args);
#else
static int YGDefaultLog(const YGConfigRef config,
                        const YGNodeRef node,
                        YGLogLevel level,
                        const char *format,
                        va_list args);
#endif

static YGConfig gYGConfigDefaults = {
    .experimentalFeatures =
        {
                [YGExperimentalFeatureWebFlexBasis] = false,
        },
    .useWebDefaults = false,
    .pointScaleFactor = 1.0f,
#ifdef ANDROID
    .logger = &YGAndroidLog,
#else
    .logger = &YGDefaultLog,
#endif
    .context = NULL,
};

static void YGNodeMarkDirtyInternal(const YGNodeRef node);

YGMalloc gYGMalloc = &malloc;
YGCalloc gYGCalloc = &calloc;
YGRealloc gYGRealloc = &realloc;
YGFree gYGFree = &free;

static YGValue YGValueZero = {.value = 0, .unit = YGUnitPoint};

#ifdef ANDROID
#include <android/log.h>
static int YGAndroidLog(const YGConfigRef config,
                        const YGNodeRef node,
                        YGLogLevel level,
                        const char *format,
                        va_list args) {
  int androidLevel = YGLogLevelDebug;
  switch (level) {
    case YGLogLevelFatal:
      androidLevel = ANDROID_LOG_FATAL;
      break;
    case YGLogLevelError:
      androidLevel = ANDROID_LOG_ERROR;
      break;
    case YGLogLevelWarn:
      androidLevel = ANDROID_LOG_WARN;
      break;
    case YGLogLevelInfo:
      androidLevel = ANDROID_LOG_INFO;
      break;
    case YGLogLevelDebug:
      androidLevel = ANDROID_LOG_DEBUG;
      break;
    case YGLogLevelVerbose:
      androidLevel = ANDROID_LOG_VERBOSE;
      break;
  }
  const int result = __android_log_vprint(androidLevel, "yoga", format, args);
  return result;
}
#else
static int YGDefaultLog(const YGConfigRef config,
                        const YGNodeRef node,
                        YGLogLevel level,
                        const char *format,
                        va_list args) {
  switch (level) {
    case YGLogLevelError:
    case YGLogLevelFatal:
      return vfprintf(stderr, format, args);
    case YGLogLevelWarn:
    case YGLogLevelInfo:
    case YGLogLevelDebug:
    case YGLogLevelVerbose:
    default:
      return vprintf(format, args);
  }
}
#endif

static inline const YGValue *YGComputedEdgeValue(const YGValue edges[YGEdgeCount],
                                                 const YGEdge edge,
                                                 const YGValue *const defaultValue) {
  if (edges[edge].unit != YGUnitUndefined) {
    return &edges[edge];
  }

  if ((edge == YGEdgeTop || edge == YGEdgeBottom) &&
      edges[YGEdgeVertical].unit != YGUnitUndefined) {
    return &edges[YGEdgeVertical];
  }

  if ((edge == YGEdgeLeft || edge == YGEdgeRight || edge == YGEdgeStart || edge == YGEdgeEnd) &&
      edges[YGEdgeHorizontal].unit != YGUnitUndefined) {
    return &edges[YGEdgeHorizontal];
  }

  if (edges[YGEdgeAll].unit != YGUnitUndefined) {
    return &edges[YGEdgeAll];
  }

  if (edge == YGEdgeStart || edge == YGEdgeEnd) {
    return &YGValueUndefined;
  }

  return defaultValue;
}

static inline float YGResolveValue(const YGValue *const value, const float parentSize) {
  switch (value->unit) {
    case YGUnitUndefined:
    case YGUnitAuto:
      return YGUndefined;
    case YGUnitPoint:
      return value->value;
    case YGUnitPercent:
      return value->value * parentSize / 100.0f;
  }
  return YGUndefined;
}

static inline float YGResolveValueMargin(const YGValue *const value, const float parentSize) {
  return value->unit == YGUnitAuto ? 0 : YGResolveValue(value, parentSize);
}

int32_t gNodeInstanceCount = 0;
int32_t gConfigInstanceCount = 0;

WIN_EXPORT YGNodeRef YGNodeNewWithConfig(const YGConfigRef config) {
  const YGNodeRef node = gYGMalloc(sizeof(YGNode));
  YGAssertWithConfig(config, node != NULL, "Could not allocate memory for node");
  gNodeInstanceCount++;

  memcpy(node, &gYGNodeDefaults, sizeof(YGNode));
  if (config->useWebDefaults) {
    node->style.flexDirection = YGFlexDirectionRow;
    node->style.alignContent = YGAlignStretch;
  }
  node->config = config;
  return node;
}

YGNodeRef YGNodeNew(void) {
  return YGNodeNewWithConfig(&gYGConfigDefaults);
}

void YGNodeFree(const YGNodeRef node) {
  if (node->parent) {
    YGNodeListDelete(node->parent->children, node);
    node->parent = NULL;
  }

  const uint32_t childCount = YGNodeGetChildCount(node);
  for (uint32_t i = 0; i < childCount; i++) {
    const YGNodeRef child = YGNodeGetChild(node, i);
    child->parent = NULL;
  }

  YGNodeListFree(node->children);
  gYGFree(node);
  gNodeInstanceCount--;
}

void YGNodeFreeRecursive(const YGNodeRef root) {
  while (YGNodeGetChildCount(root) > 0) {
    const YGNodeRef child = YGNodeGetChild(root, 0);
    YGNodeRemoveChild(root, child);
    YGNodeFreeRecursive(child);
  }
  YGNodeFree(root);
}

void YGNodeReset(const YGNodeRef node) {
  YGAssertWithNode(node,
                   YGNodeGetChildCount(node) == 0,
                   "Cannot reset a node which still has children attached");
  YGAssertWithNode(node, node->parent == NULL, "Cannot reset a node still attached to a parent");

  YGNodeListFree(node->children);

  const YGConfigRef config = node->config;
  memcpy(node, &gYGNodeDefaults, sizeof(YGNode));
  if (config->useWebDefaults) {
    node->style.flexDirection = YGFlexDirectionRow;
    node->style.alignContent = YGAlignStretch;
  }
  node->config = config;
}

int32_t YGNodeGetInstanceCount(void) {
  return gNodeInstanceCount;
}

int32_t YGConfigGetInstanceCount(void) {
  return gConfigInstanceCount;
}

// Export only for C#
YGConfigRef YGConfigGetDefault() {
  return &gYGConfigDefaults;
}

YGConfigRef YGConfigNew(void) {
  const YGConfigRef config = gYGMalloc(sizeof(YGConfig));
  YGAssert(config != NULL, "Could not allocate memory for config");

  gConfigInstanceCount++;
  memcpy(config, &gYGConfigDefaults, sizeof(YGConfig));
  return config;
}

void YGConfigFree(const YGConfigRef config) {
  gYGFree(config);
  gConfigInstanceCount--;
}

void YGConfigCopy(const YGConfigRef dest, const YGConfigRef src) {
  memcpy(dest, src, sizeof(YGConfig));
}

static void YGNodeMarkDirtyInternal(const YGNodeRef node) {
  if (!node->isDirty) {
    node->isDirty = true;
    node->layout.computedFlexBasis = YGUndefined;
    if (node->parent) {
      YGNodeMarkDirtyInternal(node->parent);
    }
  }
}

void YGNodeSetMeasureFunc(const YGNodeRef node, YGMeasureFunc measureFunc) {
  if (measureFunc == NULL) {
    node->measure = NULL;
    // TODO: t18095186 Move nodeType to opt-in function and mark appropriate places in Litho
    node->nodeType = YGNodeTypeDefault;
  } else {
    YGAssertWithNode(
        node,
        YGNodeGetChildCount(node) == 0,
        "Cannot set measure function: Nodes with measure functions cannot have children.");
    node->measure = measureFunc;
    // TODO: t18095186 Move nodeType to opt-in function and mark appropriate places in Litho
    node->nodeType = YGNodeTypeText;
  }
}

YGMeasureFunc YGNodeGetMeasureFunc(const YGNodeRef node) {
  return node->measure;
}

void YGNodeSetBaselineFunc(const YGNodeRef node, YGBaselineFunc baselineFunc) {
  node->baseline = baselineFunc;
}

YGBaselineFunc YGNodeGetBaselineFunc(const YGNodeRef node) {
  return node->baseline;
}

void YGNodeInsertChild(const YGNodeRef node, const YGNodeRef child, const uint32_t index) {
  YGAssertWithNode(node,
                   child->parent == NULL,
                   "Child already has a parent, it must be removed first.");
  YGAssertWithNode(node,
                   node->measure == NULL,
                   "Cannot add child: Nodes with measure functions cannot have children.");

  YGNodeListInsert(&node->children, child, index);
  child->parent = node;
  YGNodeMarkDirtyInternal(node);
}

void YGNodeRemoveChild(const YGNodeRef node, const YGNodeRef child) {
  if (YGNodeListDelete(node->children, child) != NULL) {
    child->layout = gYGNodeDefaults.layout; // layout is no longer valid
    child->parent = NULL;
    YGNodeMarkDirtyInternal(node);
  }
}

YGNodeRef YGNodeGetChild(const YGNodeRef node, const uint32_t index) {
  return YGNodeListGet(node->children, index);
}

YGNodeRef YGNodeGetParent(const YGNodeRef node) {
  return node->parent;
}

inline uint32_t YGNodeGetChildCount(const YGNodeRef node) {
  return YGNodeListCount(node->children);
}

void YGNodeMarkDirty(const YGNodeRef node) {
  YGAssertWithNode(node,
                   node->measure != NULL,
                   "Only leaf nodes with custom measure functions"
                   "should manually mark themselves as dirty");

  YGNodeMarkDirtyInternal(node);
}

bool YGNodeIsDirty(const YGNodeRef node) {
  return node->isDirty;
}

void YGNodeCopyStyle(const YGNodeRef dstNode, const YGNodeRef srcNode) {
  if (memcmp(&dstNode->style, &srcNode->style, sizeof(YGStyle)) != 0) {
    memcpy(&dstNode->style, &srcNode->style, sizeof(YGStyle));
    YGNodeMarkDirtyInternal(dstNode);
  }
}

static inline float YGResolveFlexGrow(const YGNodeRef node) {
  // Root nodes flexGrow should always be 0
  if (node->parent == NULL) {
    return 0.0;
  }
  if (!YGFloatIsUndefined(node->style.flexGrow)) {
    return node->style.flexGrow;
  }
  if (!YGFloatIsUndefined(node->style.flex) && node->style.flex > 0.0f) {
    return node->style.flex;
  }
  return kDefaultFlexGrow;
}

float YGNodeStyleGetFlexGrow(const YGNodeRef node) {
  return YGFloatIsUndefined(node->style.flexGrow) ? kDefaultFlexGrow : node->style.flexGrow;
}

float YGNodeStyleGetFlexShrink(const YGNodeRef node) {
  return YGFloatIsUndefined(node->style.flexShrink)
             ? (node->config->useWebDefaults ? kWebDefaultFlexShrink : kDefaultFlexShrink)
             : node->style.flexShrink;
}

static inline float YGNodeResolveFlexShrink(const YGNodeRef node) {
  // Root nodes flexShrink should always be 0
  if (node->parent == NULL) {
    return 0.0;
  }
  if (!YGFloatIsUndefined(node->style.flexShrink)) {
    return node->style.flexShrink;
  }
  // 如果 style.flex 为负值则表示 shrink 值（非web标准下）
  if (!node->config->useWebDefaults && !YGFloatIsUndefined(node->style.flex) &&
      node->style.flex < 0.0f) {
    return -node->style.flex;
  }
  // web 标准默认 shrink 为 1, yoga 为 0
  return node->config->useWebDefaults ? kWebDefaultFlexShrink : kDefaultFlexShrink;
}

static inline const YGValue *YGNodeResolveFlexBasisPtr(const YGNodeRef node) {
  if (node->style.flexBasis.unit != YGUnitAuto && node->style.flexBasis.unit != YGUnitUndefined) {
    return &node->style.flexBasis;
  }
  if (!YGFloatIsUndefined(node->style.flex) && node->style.flex > 0.0f) {
    return node->config->useWebDefaults ? &YGValueAuto : &YGValueZero;
  }
  return &YGValueAuto;
}

#define YG_NODE_PROPERTY_IMPL(type, name, paramName, instanceName) \
  void YGNodeSet##name(const YGNodeRef node, type paramName) {     \
    node->instanceName = paramName;                                \
  }                                                                \
                                                                   \
  type YGNodeGet##name(const YGNodeRef node) {                     \
    return node->instanceName;                                     \
  }

#define YG_NODE_STYLE_PROPERTY_SETTER_IMPL(type, name, paramName, instanceName) \
  void YGNodeStyleSet##name(const YGNodeRef node, const type paramName) {       \
    if (node->style.instanceName != paramName) {                                \
      node->style.instanceName = paramName;                                     \
      YGNodeMarkDirtyInternal(node);                                            \
    }                                                                           \
  }

#define YG_NODE_STYLE_PROPERTY_SETTER_UNIT_IMPL(type, name, paramName, instanceName)              \
  void YGNodeStyleSet##name(const YGNodeRef node, const type paramName) {                         \
    if (node->style.instanceName.value != paramName ||                                            \
        node->style.instanceName.unit != YGUnitPoint) {                                           \
      node->style.instanceName.value = paramName;                                                 \
      node->style.instanceName.unit = YGFloatIsUndefined(paramName) ? YGUnitAuto : YGUnitPoint;   \
      YGNodeMarkDirtyInternal(node);                                                              \
    }                                                                                             \
  }                                                                                               \
                                                                                                  \
  void YGNodeStyleSet##name##Percent(const YGNodeRef node, const type paramName) {                \
    if (node->style.instanceName.value != paramName ||                                            \
        node->style.instanceName.unit != YGUnitPercent) {                                         \
      node->style.instanceName.value = paramName;                                                 \
      node->style.instanceName.unit = YGFloatIsUndefined(paramName) ? YGUnitAuto : YGUnitPercent; \
      YGNodeMarkDirtyInternal(node);                                                              \
    }                                                                                             \
  }

#define YG_NODE_STYLE_PROPERTY_SETTER_UNIT_AUTO_IMPL(type, name, paramName, instanceName)         \
  void YGNodeStyleSet##name(const YGNodeRef node, const type paramName) {                         \
    if (node->style.instanceName.value != paramName ||                                            \
        node->style.instanceName.unit != YGUnitPoint) {                                           \
      node->style.instanceName.value = paramName;                                                 \
      node->style.instanceName.unit = YGFloatIsUndefined(paramName) ? YGUnitAuto : YGUnitPoint;   \
      YGNodeMarkDirtyInternal(node);                                                              \
    }                                                                                             \
  }                                                                                               \
                                                                                                  \
  void YGNodeStyleSet##name##Percent(const YGNodeRef node, const type paramName) {                \
    if (node->style.instanceName.value != paramName ||                                            \
        node->style.instanceName.unit != YGUnitPercent) {                                         \
      node->style.instanceName.value = paramName;                                                 \
      node->style.instanceName.unit = YGFloatIsUndefined(paramName) ? YGUnitAuto : YGUnitPercent; \
      YGNodeMarkDirtyInternal(node);                                                              \
    }                                                                                             \
  }                                                                                               \
                                                                                                  \
  void YGNodeStyleSet##name##Auto(const YGNodeRef node) {                                         \
    if (node->style.instanceName.unit != YGUnitAuto) {                                            \
      node->style.instanceName.value = YGUndefined;                                               \
      node->style.instanceName.unit = YGUnitAuto;                                                 \
      YGNodeMarkDirtyInternal(node);                                                              \
    }                                                                                             \
  }

#define YG_NODE_STYLE_PROPERTY_IMPL(type, name, paramName, instanceName)  \
  YG_NODE_STYLE_PROPERTY_SETTER_IMPL(type, name, paramName, instanceName) \
                                                                          \
  type YGNodeStyleGet##name(const YGNodeRef node) {                       \
    return node->style.instanceName;                                      \
  }

#define YG_NODE_STYLE_PROPERTY_UNIT_IMPL(type, name, paramName, instanceName)   \
  YG_NODE_STYLE_PROPERTY_SETTER_UNIT_IMPL(float, name, paramName, instanceName) \
                                                                                \
  type YGNodeStyleGet##name(const YGNodeRef node) {                             \
    return node->style.instanceName;                                            \
  }

#define YG_NODE_STYLE_PROPERTY_UNIT_AUTO_IMPL(type, name, paramName, instanceName)   \
  YG_NODE_STYLE_PROPERTY_SETTER_UNIT_AUTO_IMPL(float, name, paramName, instanceName) \
                                                                                     \
  type YGNodeStyleGet##name(const YGNodeRef node) {                                  \
    return node->style.instanceName;                                                 \
  }

#define YG_NODE_STYLE_EDGE_PROPERTY_UNIT_AUTO_IMPL(type, name, instanceName) \
  void YGNodeStyleSet##name##Auto(const YGNodeRef node, const YGEdge edge) { \
    if (node->style.instanceName[edge].unit != YGUnitAuto) {                 \
      node->style.instanceName[edge].value = YGUndefined;                    \
      node->style.instanceName[edge].unit = YGUnitAuto;                      \
      YGNodeMarkDirtyInternal(node);                                         \
    }                                                                        \
  }

#define YG_NODE_STYLE_EDGE_PROPERTY_UNIT_IMPL(type, name, paramName, instanceName)            \
  void YGNodeStyleSet##name(const YGNodeRef node, const YGEdge edge, const float paramName) { \
    if (node->style.instanceName[edge].value != paramName ||                                  \
        node->style.instanceName[edge].unit != YGUnitPoint) {                                 \
      node->style.instanceName[edge].value = paramName;                                       \
      node->style.instanceName[edge].unit =                                                   \
          YGFloatIsUndefined(paramName) ? YGUnitUndefined : YGUnitPoint;                      \
      YGNodeMarkDirtyInternal(node);                                                          \
    }                                                                                         \
  }                                                                                           \
                                                                                              \
  void YGNodeStyleSet##name##Percent(const YGNodeRef node,                                    \
                                     const YGEdge edge,                                       \
                                     const float paramName) {                                 \
    if (node->style.instanceName[edge].value != paramName ||                                  \
        node->style.instanceName[edge].unit != YGUnitPercent) {                               \
      node->style.instanceName[edge].value = paramName;                                       \
      node->style.instanceName[edge].unit =                                                   \
          YGFloatIsUndefined(paramName) ? YGUnitUndefined : YGUnitPercent;                    \
      YGNodeMarkDirtyInternal(node);                                                          \
    }                                                                                         \
  }                                                                                           \
                                                                                              \
  WIN_STRUCT(type) YGNodeStyleGet##name(const YGNodeRef node, const YGEdge edge) {            \
    return WIN_STRUCT_REF(node->style.instanceName[edge]);                                    \
  }

#define YG_NODE_STYLE_EDGE_PROPERTY_IMPL(type, name, paramName, instanceName)                 \
  void YGNodeStyleSet##name(const YGNodeRef node, const YGEdge edge, const float paramName) { \
    if (node->style.instanceName[edge].value != paramName ||                                  \
        node->style.instanceName[edge].unit != YGUnitPoint) {                                 \
      node->style.instanceName[edge].value = paramName;                                       \
      node->style.instanceName[edge].unit =                                                   \
          YGFloatIsUndefined(paramName) ? YGUnitUndefined : YGUnitPoint;                      \
      YGNodeMarkDirtyInternal(node);                                                          \
    }                                                                                         \
  }                                                                                           \
                                                                                              \
  float YGNodeStyleGet##name(const YGNodeRef node, const YGEdge edge) {                       \
    return node->style.instanceName[edge].value;                                              \
  }

#define YG_NODE_LAYOUT_PROPERTY_IMPL(type, name, instanceName) \
  type YGNodeLayoutGet##name(const YGNodeRef node) {           \
    return node->layout.instanceName;                          \
  }

#define YG_NODE_LAYOUT_RESOLVED_PROPERTY_IMPL(type, name, instanceName)        \
  type YGNodeLayoutGet##name(const YGNodeRef node, const YGEdge edge) {        \
    YGAssertWithNode(node,                                                     \
                     edge < YGEdgeEnd,                                         \
                     "Cannot get layout properties of multi-edge shorthands"); \
                                                                               \
    if (edge == YGEdgeLeft) {                                                  \
      if (node->layout.direction == YGDirectionRTL) {                          \
        return node->layout.instanceName[YGEdgeEnd];                           \
      } else {                                                                 \
        return node->layout.instanceName[YGEdgeStart];                         \
      }                                                                        \
    }                                                                          \
                                                                               \
    if (edge == YGEdgeRight) {                                                 \
      if (node->layout.direction == YGDirectionRTL) {                          \
        return node->layout.instanceName[YGEdgeStart];                         \
      } else {                                                                 \
        return node->layout.instanceName[YGEdgeEnd];                           \
      }                                                                        \
    }                                                                          \
                                                                               \
    return node->layout.instanceName[edge];                                    \
  }

YG_NODE_PROPERTY_IMPL(void *, Context, context, context);
YG_NODE_PROPERTY_IMPL(YGPrintFunc, PrintFunc, printFunc, print);
YG_NODE_PROPERTY_IMPL(bool, HasNewLayout, hasNewLayout, hasNewLayout);
YG_NODE_PROPERTY_IMPL(YGNodeType, NodeType, nodeType, nodeType);

YG_NODE_STYLE_PROPERTY_IMPL(YGDirection, Direction, direction, direction);
YG_NODE_STYLE_PROPERTY_IMPL(YGFlexDirection, FlexDirection, flexDirection, flexDirection);
YG_NODE_STYLE_PROPERTY_IMPL(YGJustify, JustifyContent, justifyContent, justifyContent);
YG_NODE_STYLE_PROPERTY_IMPL(YGAlign, AlignContent, alignContent, alignContent);
YG_NODE_STYLE_PROPERTY_IMPL(YGAlign, AlignItems, alignItems, alignItems);
YG_NODE_STYLE_PROPERTY_IMPL(YGAlign, AlignSelf, alignSelf, alignSelf);
YG_NODE_STYLE_PROPERTY_IMPL(YGPositionType, PositionType, positionType, positionType);
YG_NODE_STYLE_PROPERTY_IMPL(YGWrap, FlexWrap, flexWrap, flexWrap);
YG_NODE_STYLE_PROPERTY_IMPL(YGOverflow, Overflow, overflow, overflow);
YG_NODE_STYLE_PROPERTY_IMPL(YGDisplay, Display, display, display);

YG_NODE_STYLE_PROPERTY_IMPL(float, Flex, flex, flex);
YG_NODE_STYLE_PROPERTY_SETTER_IMPL(float, FlexGrow, flexGrow, flexGrow);
YG_NODE_STYLE_PROPERTY_SETTER_IMPL(float, FlexShrink, flexShrink, flexShrink);
YG_NODE_STYLE_PROPERTY_UNIT_AUTO_IMPL(YGValue, FlexBasis, flexBasis, flexBasis);

YG_NODE_STYLE_EDGE_PROPERTY_UNIT_IMPL(YGValue, Position, position, position);
YG_NODE_STYLE_EDGE_PROPERTY_UNIT_IMPL(YGValue, Margin, margin, margin);
YG_NODE_STYLE_EDGE_PROPERTY_UNIT_AUTO_IMPL(YGValue, Margin, margin);
YG_NODE_STYLE_EDGE_PROPERTY_UNIT_IMPL(YGValue, Padding, padding, padding);
YG_NODE_STYLE_EDGE_PROPERTY_IMPL(float, Border, border, border);

YG_NODE_STYLE_PROPERTY_UNIT_AUTO_IMPL(YGValue, Width, width, dimensions[YGDimensionWidth]);
YG_NODE_STYLE_PROPERTY_UNIT_AUTO_IMPL(YGValue, Height, height, dimensions[YGDimensionHeight]);
YG_NODE_STYLE_PROPERTY_UNIT_IMPL(YGValue, MinWidth, minWidth, minDimensions[YGDimensionWidth]);
YG_NODE_STYLE_PROPERTY_UNIT_IMPL(YGValue, MinHeight, minHeight, minDimensions[YGDimensionHeight]);
YG_NODE_STYLE_PROPERTY_UNIT_IMPL(YGValue, MaxWidth, maxWidth, maxDimensions[YGDimensionWidth]);
YG_NODE_STYLE_PROPERTY_UNIT_IMPL(YGValue, MaxHeight, maxHeight, maxDimensions[YGDimensionHeight]);

// Yoga specific properties, not compatible with flexbox specification
YG_NODE_STYLE_PROPERTY_IMPL(float, AspectRatio, aspectRatio, aspectRatio);

YG_NODE_LAYOUT_PROPERTY_IMPL(float, Left, position[YGEdgeLeft]);
YG_NODE_LAYOUT_PROPERTY_IMPL(float, Top, position[YGEdgeTop]);
YG_NODE_LAYOUT_PROPERTY_IMPL(float, Right, position[YGEdgeRight]);
YG_NODE_LAYOUT_PROPERTY_IMPL(float, Bottom, position[YGEdgeBottom]);
YG_NODE_LAYOUT_PROPERTY_IMPL(float, Width, dimensions[YGDimensionWidth]);
YG_NODE_LAYOUT_PROPERTY_IMPL(float, Height, dimensions[YGDimensionHeight]);
YG_NODE_LAYOUT_PROPERTY_IMPL(YGDirection, Direction, direction);
YG_NODE_LAYOUT_PROPERTY_IMPL(bool, HadOverflow, hadOverflow);

YG_NODE_LAYOUT_RESOLVED_PROPERTY_IMPL(float, Margin, margin);
YG_NODE_LAYOUT_RESOLVED_PROPERTY_IMPL(float, Border, border);
YG_NODE_LAYOUT_RESOLVED_PROPERTY_IMPL(float, Padding, padding);

uint32_t gCurrentGenerationCount = 0;

bool YGLayoutNodeInternal(const YGNodeRef node,
                          const float availableWidth,
                          const float availableHeight,
                          const YGDirection parentDirection,
                          const YGMeasureMode widthMeasureMode,
                          const YGMeasureMode heightMeasureMode,
                          const float parentWidth,
                          const float parentHeight,
                          const bool performLayout,
                          const char *reason,
                          const YGConfigRef config);

inline bool YGFloatIsUndefined(const float value) {
  return isnan(value);
}

static inline bool YGValueEqual(const YGValue a, const YGValue b) {
  if (a.unit != b.unit) {
    return false;
  }

  if (a.unit == YGUnitUndefined) {
    return true;
  }

  return fabs(a.value - b.value) < 0.0001f;
}

static inline void YGResolveDimensions(YGNodeRef node) {
  for (YGDimension dim = YGDimensionWidth; dim <= YGDimensionHeight; dim++) {
    if (node->style.maxDimensions[dim].unit != YGUnitUndefined &&
        YGValueEqual(node->style.maxDimensions[dim], node->style.minDimensions[dim])) {
      node->resolvedDimensions[dim] = &node->style.maxDimensions[dim];
    } else {
      node->resolvedDimensions[dim] = &node->style.dimensions[dim];
    }
  }
}

static inline bool YGFloatsEqual(const float a, const float b) {
  if (YGFloatIsUndefined(a)) {
    return YGFloatIsUndefined(b);
  }
  return fabs(a - b) < 0.0001f;
}

static void YGIndent(const YGNodeRef node, const uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    YGLog(node, YGLogLevelDebug, "  ");
  }
}

static void YGPrintNumberIfNotUndefinedf(const YGNodeRef node,
                                         const char *str,
                                         const float number) {
  if (!YGFloatIsUndefined(number)) {
    YGLog(node, YGLogLevelDebug, "%s: %g; ", str, number);
  }
}

static void YGPrintNumberIfNotUndefined(const YGNodeRef node,
                                        const char *str,
                                        const YGValue *const number) {
  if (number->unit != YGUnitUndefined) {
    if (number->unit == YGUnitAuto) {
      YGLog(node, YGLogLevelDebug, "%s: auto; ", str);
    } else {
      const char *unit = number->unit == YGUnitPoint ? "px" : "%";
      YGLog(node, YGLogLevelDebug, "%s: %g%s; ", str, number->value, unit);
    }
  }
}

static void YGPrintNumberIfNotAuto(const YGNodeRef node,
                                   const char *str,
                                   const YGValue *const number) {
  if (number->unit != YGUnitAuto) {
    YGPrintNumberIfNotUndefined(node, str, number);
  }
}

static void YGPrintEdgeIfNotUndefined(const YGNodeRef node,
                                      const char *str,
                                      const YGValue *edges,
                                      const YGEdge edge) {
  YGPrintNumberIfNotUndefined(node, str, YGComputedEdgeValue(edges, edge, &YGValueUndefined));
}

static void YGPrintNumberIfNotZero(const YGNodeRef node,
                                   const char *str,
                                   const YGValue *const number) {
  if (!YGFloatsEqual(number->value, 0)) {
    YGPrintNumberIfNotUndefined(node, str, number);
  }
}

static bool YGFourValuesEqual(const YGValue four[4]) {
  return YGValueEqual(four[0], four[1]) && YGValueEqual(four[0], four[2]) &&
         YGValueEqual(four[0], four[3]);
}

static void YGPrintEdges(const YGNodeRef node, const char *str, const YGValue *edges) {
  if (YGFourValuesEqual(edges)) {
    YGPrintNumberIfNotZero(node, str, &edges[YGEdgeLeft]);
  } else {
    for (YGEdge edge = YGEdgeLeft; edge < YGEdgeCount; edge++) {
      char buf[30];
      snprintf(buf, sizeof(buf), "%s-%s", str, YGEdgeToString(edge));
      YGPrintNumberIfNotZero(node, buf, &edges[edge]);
    }
  }
}

static void YGNodePrintInternal(const YGNodeRef node,
                                const YGPrintOptions options,
                                const uint32_t level) {
  YGIndent(node, level);
  YGLog(node, YGLogLevelDebug, "<div ");

  if (node->print) {
    node->print(node);
  }

  if (options & YGPrintOptionsLayout) {
    YGLog(node, YGLogLevelDebug, "layout=\"");
    YGLog(node, YGLogLevelDebug, "width: %g; ", node->layout.dimensions[YGDimensionWidth]);
    YGLog(node, YGLogLevelDebug, "height: %g; ", node->layout.dimensions[YGDimensionHeight]);
    YGLog(node, YGLogLevelDebug, "top: %g; ", node->layout.position[YGEdgeTop]);
    YGLog(node, YGLogLevelDebug, "left: %g;", node->layout.position[YGEdgeLeft]);
    YGLog(node, YGLogLevelDebug, "\" ");
  }

  if (options & YGPrintOptionsStyle) {
    YGLog(node, YGLogLevelDebug, "style=\"");
    if (node->style.flexDirection != gYGNodeDefaults.style.flexDirection) {
      YGLog(node,
            YGLogLevelDebug,
            "flex-direction: %s; ",
            YGFlexDirectionToString(node->style.flexDirection));
    }
    if (node->style.justifyContent != gYGNodeDefaults.style.justifyContent) {
      YGLog(node,
            YGLogLevelDebug,
            "justify-content: %s; ",
            YGJustifyToString(node->style.justifyContent));
    }
    if (node->style.alignItems != gYGNodeDefaults.style.alignItems) {
      YGLog(node, YGLogLevelDebug, "align-items: %s; ", YGAlignToString(node->style.alignItems));
    }
    if (node->style.alignContent != gYGNodeDefaults.style.alignContent) {
      YGLog(node, YGLogLevelDebug, "align-content: %s; ", YGAlignToString(node->style.alignContent));
    }
    if (node->style.alignSelf != gYGNodeDefaults.style.alignSelf) {
      YGLog(node, YGLogLevelDebug, "align-self: %s; ", YGAlignToString(node->style.alignSelf));
    }

    YGPrintNumberIfNotUndefinedf(node, "flex-grow", node->style.flexGrow);
    YGPrintNumberIfNotUndefinedf(node, "flex-shrink", node->style.flexShrink);
    YGPrintNumberIfNotAuto(node, "flex-basis", &node->style.flexBasis);
    YGPrintNumberIfNotUndefinedf(node, "flex", node->style.flex);

    if (node->style.flexWrap != gYGNodeDefaults.style.flexWrap) {
      YGLog(node, YGLogLevelDebug, "flexWrap: %s; ", YGWrapToString(node->style.flexWrap));
    }

    if (node->style.overflow != gYGNodeDefaults.style.overflow) {
      YGLog(node, YGLogLevelDebug, "overflow: %s; ", YGOverflowToString(node->style.overflow));
    }

    if (node->style.display != gYGNodeDefaults.style.display) {
      YGLog(node, YGLogLevelDebug, "display: %s; ", YGDisplayToString(node->style.display));
    }

    YGPrintEdges(node, "margin", node->style.margin);
    YGPrintEdges(node, "padding", node->style.padding);
    YGPrintEdges(node, "border", node->style.border);

    YGPrintNumberIfNotAuto(node, "width", &node->style.dimensions[YGDimensionWidth]);
    YGPrintNumberIfNotAuto(node, "height", &node->style.dimensions[YGDimensionHeight]);
    YGPrintNumberIfNotAuto(node, "max-width", &node->style.maxDimensions[YGDimensionWidth]);
    YGPrintNumberIfNotAuto(node, "max-height", &node->style.maxDimensions[YGDimensionHeight]);
    YGPrintNumberIfNotAuto(node, "min-width", &node->style.minDimensions[YGDimensionWidth]);
    YGPrintNumberIfNotAuto(node, "min-height", &node->style.minDimensions[YGDimensionHeight]);

    if (node->style.positionType != gYGNodeDefaults.style.positionType) {
      YGLog(node,
            YGLogLevelDebug,
            "position: %s; ",
            YGPositionTypeToString(node->style.positionType));
    }

    YGPrintEdgeIfNotUndefined(node, "left", node->style.position, YGEdgeLeft);
    YGPrintEdgeIfNotUndefined(node, "right", node->style.position, YGEdgeRight);
    YGPrintEdgeIfNotUndefined(node, "top", node->style.position, YGEdgeTop);
    YGPrintEdgeIfNotUndefined(node, "bottom", node->style.position, YGEdgeBottom);
    YGLog(node, YGLogLevelDebug, "\" ");

    if (node->measure != NULL) {
      YGLog(node, YGLogLevelDebug, "has-custom-measure=\"true\"");
    }
  }
  YGLog(node, YGLogLevelDebug, ">");

  const uint32_t childCount = YGNodeListCount(node->children);
  if (options & YGPrintOptionsChildren && childCount > 0) {
    for (uint32_t i = 0; i < childCount; i++) {
      YGLog(node, YGLogLevelDebug, "\n");
      YGNodePrintInternal(YGNodeGetChild(node, i), options, level + 1);
    }
    YGIndent(node, level);
    YGLog(node, YGLogLevelDebug, "\n");
  }
  YGLog(node, YGLogLevelDebug, "</div>");
}

void YGNodePrint(const YGNodeRef node, const YGPrintOptions options) {
  YGNodePrintInternal(node, options, 0);
}

static const YGEdge leading[4] = {
        [YGFlexDirectionColumn] = YGEdgeTop,
        [YGFlexDirectionColumnReverse] = YGEdgeBottom,
        [YGFlexDirectionRow] = YGEdgeLeft,
        [YGFlexDirectionRowReverse] = YGEdgeRight,
};
static const YGEdge trailing[4] = {
        [YGFlexDirectionColumn] = YGEdgeBottom,
        [YGFlexDirectionColumnReverse] = YGEdgeTop,
        [YGFlexDirectionRow] = YGEdgeRight,
        [YGFlexDirectionRowReverse] = YGEdgeLeft,
};
static const YGEdge pos[4] = {
        [YGFlexDirectionColumn] = YGEdgeTop,
        [YGFlexDirectionColumnReverse] = YGEdgeBottom,
        [YGFlexDirectionRow] = YGEdgeLeft,
        [YGFlexDirectionRowReverse] = YGEdgeRight,
};
static const YGDimension dim[4] = {
        [YGFlexDirectionColumn] = YGDimensionHeight,
        [YGFlexDirectionColumnReverse] = YGDimensionHeight,
        [YGFlexDirectionRow] = YGDimensionWidth,
        [YGFlexDirectionRowReverse] = YGDimensionWidth,
};

static inline bool YGFlexDirectionIsRow(const YGFlexDirection flexDirection) {
  return flexDirection == YGFlexDirectionRow || flexDirection == YGFlexDirectionRowReverse;
}

static inline bool YGFlexDirectionIsColumn(const YGFlexDirection flexDirection) {
  return flexDirection == YGFlexDirectionColumn || flexDirection == YGFlexDirectionColumnReverse;
}

static inline float YGNodeLeadingMargin(const YGNodeRef node,
                                        const YGFlexDirection axis,
                                        const float widthSize) {
  if (YGFlexDirectionIsRow(axis) && node->style.margin[YGEdgeStart].unit != YGUnitUndefined) {
    return YGResolveValueMargin(&node->style.margin[YGEdgeStart], widthSize);
  }

  return YGResolveValueMargin(YGComputedEdgeValue(node->style.margin, leading[axis], &YGValueZero),
                              widthSize);
}

static float YGNodeTrailingMargin(const YGNodeRef node,
                                  const YGFlexDirection axis,
                                  const float widthSize) {
  if (YGFlexDirectionIsRow(axis) && node->style.margin[YGEdgeEnd].unit != YGUnitUndefined) {
    return YGResolveValueMargin(&node->style.margin[YGEdgeEnd], widthSize);
  }

  return YGResolveValueMargin(YGComputedEdgeValue(node->style.margin, trailing[axis], &YGValueZero),
                              widthSize);
}

static float YGNodeLeadingPadding(const YGNodeRef node,
                                  const YGFlexDirection axis,
                                  const float widthSize) {
  if (YGFlexDirectionIsRow(axis) && node->style.padding[YGEdgeStart].unit != YGUnitUndefined &&
      YGResolveValue(&node->style.padding[YGEdgeStart], widthSize) >= 0.0f) {
    return YGResolveValue(&node->style.padding[YGEdgeStart], widthSize);
  }

  return fmaxf(YGResolveValue(YGComputedEdgeValue(node->style.padding, leading[axis], &YGValueZero),
                              widthSize),
               0.0f);
}

static float YGNodeTrailingPadding(const YGNodeRef node,
                                   const YGFlexDirection axis,
                                   const float widthSize) {
  if (YGFlexDirectionIsRow(axis) && node->style.padding[YGEdgeEnd].unit != YGUnitUndefined &&
      YGResolveValue(&node->style.padding[YGEdgeEnd], widthSize) >= 0.0f) {
    return YGResolveValue(&node->style.padding[YGEdgeEnd], widthSize);
  }

  return fmaxf(YGResolveValue(YGComputedEdgeValue(node->style.padding, trailing[axis], &YGValueZero),
                              widthSize),
               0.0f);
}

static float YGNodeLeadingBorder(const YGNodeRef node, const YGFlexDirection axis) {
  if (YGFlexDirectionIsRow(axis) && node->style.border[YGEdgeStart].unit != YGUnitUndefined &&
      node->style.border[YGEdgeStart].value >= 0.0f) {
    return node->style.border[YGEdgeStart].value;
  }

  return fmaxf(YGComputedEdgeValue(node->style.border, leading[axis], &YGValueZero)->value, 0.0f);
}

static float YGNodeTrailingBorder(const YGNodeRef node, const YGFlexDirection axis) {
  if (YGFlexDirectionIsRow(axis) && node->style.border[YGEdgeEnd].unit != YGUnitUndefined &&
      node->style.border[YGEdgeEnd].value >= 0.0f) {
    return node->style.border[YGEdgeEnd].value;
  }

  return fmaxf(YGComputedEdgeValue(node->style.border, trailing[axis], &YGValueZero)->value, 0.0f);
}

static inline float YGNodeLeadingPaddingAndBorder(const YGNodeRef node,
                                                  const YGFlexDirection axis,
                                                  const float widthSize) {
  return YGNodeLeadingPadding(node, axis, widthSize) + YGNodeLeadingBorder(node, axis);
}

static inline float YGNodeTrailingPaddingAndBorder(const YGNodeRef node,
                                                   const YGFlexDirection axis,
                                                   const float widthSize) {
  return YGNodeTrailingPadding(node, axis, widthSize) + YGNodeTrailingBorder(node, axis);
}

static inline float YGNodeMarginForAxis(const YGNodeRef node,
                                        const YGFlexDirection axis,
                                        const float widthSize) {
  return YGNodeLeadingMargin(node, axis, widthSize) + YGNodeTrailingMargin(node, axis, widthSize);
}

static inline float YGNodePaddingAndBorderForAxis(const YGNodeRef node,
                                                  const YGFlexDirection axis,
                                                  const float widthSize) {
  return YGNodeLeadingPaddingAndBorder(node, axis, widthSize) +
         YGNodeTrailingPaddingAndBorder(node, axis, widthSize);
}

static inline YGAlign YGNodeAlignItem(const YGNodeRef node, const YGNodeRef child) {
  const YGAlign align =
      child->style.alignSelf == YGAlignAuto ? node->style.alignItems : child->style.alignSelf;
  if (align == YGAlignBaseline && YGFlexDirectionIsColumn(node->style.flexDirection)) {
    return YGAlignFlexStart;
  }
  return align;
}

static inline YGDirection YGNodeResolveDirection(const YGNodeRef node,
                                                 const YGDirection parentDirection) {
  if (node->style.direction == YGDirectionInherit) {
    return parentDirection > YGDirectionInherit ? parentDirection : YGDirectionLTR;
  } else {
    return node->style.direction;
  }
}

/**
 * 计算 node 基线位置-在计算每一行 cross size 的时候需要考虑 baseline 对齐的情况并基于 baseline 计算
 * baseline 是一个数值，指的是 baseline 的位置到其父容器顶部边界的距离，参考(evernote 笔记中的 cross size 计算)
 * 1. 这里只会计算首行子节点的 baseline(?)
 * 2. 文本节点应该有系统方法计算 baseline ?
 * 3. 忽略绝对定位的孩子节点
 * 4. 一行中只计算首个 baseline 对齐的子节点的基线，或者最后一个非绝对定位的孩子节点
 */
static float YGBaseline(const YGNodeRef node) {
  if (node->baseline != NULL) {
    // baseline 方法从哪里来，文本节点自己计算？
    const float baseline = node->baseline(node,
                                          node->layout.measuredDimensions[YGDimensionWidth],
                                          node->layout.measuredDimensions[YGDimensionHeight]);
    YGAssertWithNode(node,
                     !YGFloatIsUndefined(baseline),
                     "Expect custom baseline function to not return NaN");
    return baseline;
  }

  YGNodeRef baselineChild = NULL;
  const uint32_t childCount = YGNodeGetChildCount(node);
  for (uint32_t i = 0; i < childCount; i++) {
    const YGNodeRef child = YGNodeGetChild(node, i);
    // 只考虑第一行(baseline 对齐时只按第一行的baseline对齐)
    // https://codepen.io/millerisme/pen/qVqOyQ
    if (child->lineIndex > 0) {
      break;
    }
    // 忽略绝对定位孩子
    if (child->style.positionType == YGPositionTypeAbsolute) {
      continue;
    }
    // 选中第一个 baseline 对齐的孩子
    if (YGNodeAlignItem(node, child) == YGAlignBaseline) {
      baselineChild = child;
      break;
    }

    // 如果没有 baseline 对齐的则找最后一个非绝对定位的孩子
    if (baselineChild == NULL) {
      baselineChild = child;
    }
  }


  // 如果没有孩子节点则其 baseline 就是它的高度
  if (baselineChild == NULL) {
    return node->layout.measuredDimensions[YGDimensionHeight];
  }

  // 递归计算孩子节点的 baseline 值（递归的终止条件是：1. node 有自己的 baseline 函数 2. node 没有孩子节点）
  const float baseline = YGBaseline(baselineChild);

  // baseline 值 + 节点顶部的位置（这应该也是一个位置值）
  return baseline + baselineChild->layout.position[YGEdgeTop];
}

static inline YGFlexDirection YGResolveFlexDirection(const YGFlexDirection flexDirection,
                                                     const YGDirection direction) {
  if (direction == YGDirectionRTL) {
    if (flexDirection == YGFlexDirectionRow) {
      return YGFlexDirectionRowReverse;
    } else if (flexDirection == YGFlexDirectionRowReverse) {
      return YGFlexDirectionRow;
    }
  }

  return flexDirection;
}

static YGFlexDirection YGFlexDirectionCross(const YGFlexDirection flexDirection,
                                            const YGDirection direction) {
  return YGFlexDirectionIsColumn(flexDirection)
             ? YGResolveFlexDirection(YGFlexDirectionRow, direction)
             : YGFlexDirectionColumn;
}

static inline bool YGNodeIsFlex(const YGNodeRef node) {
  return (node->style.positionType == YGPositionTypeRelative &&
          (YGResolveFlexGrow(node) != 0 || YGNodeResolveFlexShrink(node) != 0));
}

// node 是否按 baseline 对齐，只有行布局才考虑
static bool YGIsBaselineLayout(const YGNodeRef node) {
  if (YGFlexDirectionIsColumn(node->style.flexDirection)) {
    return false;
  }
  if (node->style.alignItems == YGAlignBaseline) {
    return true;
  }
  const uint32_t childCount = YGNodeGetChildCount(node);
  for (uint32_t i = 0; i < childCount; i++) {
    const YGNodeRef child = YGNodeGetChild(node, i);
    if (child->style.positionType == YGPositionTypeRelative &&
        child->style.alignSelf == YGAlignBaseline) {
      return true;
    }
  }

  return false;
}

static inline float YGNodeDimWithMargin(const YGNodeRef node,
                                        const YGFlexDirection axis,
                                        const float widthSize) {
  return node->layout.measuredDimensions[dim[axis]] + YGNodeLeadingMargin(node, axis, widthSize) +
         YGNodeTrailingMargin(node, axis, widthSize);
}

static inline bool YGNodeIsStyleDimDefined(const YGNodeRef node,
                                           const YGFlexDirection axis,
                                           const float parentSize) {
  return !(node->resolvedDimensions[dim[axis]]->unit == YGUnitAuto ||
           node->resolvedDimensions[dim[axis]]->unit == YGUnitUndefined ||
           (node->resolvedDimensions[dim[axis]]->unit == YGUnitPoint &&
            node->resolvedDimensions[dim[axis]]->value < 0.0f) ||
           (node->resolvedDimensions[dim[axis]]->unit == YGUnitPercent &&
            (node->resolvedDimensions[dim[axis]]->value < 0.0f || YGFloatIsUndefined(parentSize))));
}

static inline bool YGNodeIsLayoutDimDefined(const YGNodeRef node, const YGFlexDirection axis) {
  const float value = node->layout.measuredDimensions[dim[axis]];
  return !YGFloatIsUndefined(value) && value >= 0.0f;
}

static inline bool YGNodeIsLeadingPosDefined(const YGNodeRef node, const YGFlexDirection axis) {
  return (YGFlexDirectionIsRow(axis) &&
          YGComputedEdgeValue(node->style.position, YGEdgeStart, &YGValueUndefined)->unit !=
              YGUnitUndefined) ||
         YGComputedEdgeValue(node->style.position, leading[axis], &YGValueUndefined)->unit !=
             YGUnitUndefined;
}

static inline bool YGNodeIsTrailingPosDefined(const YGNodeRef node, const YGFlexDirection axis) {
  return (YGFlexDirectionIsRow(axis) &&
          YGComputedEdgeValue(node->style.position, YGEdgeEnd, &YGValueUndefined)->unit !=
              YGUnitUndefined) ||
         YGComputedEdgeValue(node->style.position, trailing[axis], &YGValueUndefined)->unit !=
             YGUnitUndefined;
}

static float YGNodeLeadingPosition(const YGNodeRef node,
                                   const YGFlexDirection axis,
                                   const float axisSize) {
  if (YGFlexDirectionIsRow(axis)) {
    const YGValue *leadingPosition =
        YGComputedEdgeValue(node->style.position, YGEdgeStart, &YGValueUndefined);
    if (leadingPosition->unit != YGUnitUndefined) {
      return YGResolveValue(leadingPosition, axisSize);
    }
  }

  const YGValue *leadingPosition =
      YGComputedEdgeValue(node->style.position, leading[axis], &YGValueUndefined);

  return leadingPosition->unit == YGUnitUndefined ? 0.0f
                                                  : YGResolveValue(leadingPosition, axisSize);
}

static float YGNodeTrailingPosition(const YGNodeRef node,
                                    const YGFlexDirection axis,
                                    const float axisSize) {
  if (YGFlexDirectionIsRow(axis)) {
    const YGValue *trailingPosition =
        YGComputedEdgeValue(node->style.position, YGEdgeEnd, &YGValueUndefined);
    if (trailingPosition->unit != YGUnitUndefined) {
      return YGResolveValue(trailingPosition, axisSize);
    }
  }

  const YGValue *trailingPosition =
      YGComputedEdgeValue(node->style.position, trailing[axis], &YGValueUndefined);

  return trailingPosition->unit == YGUnitUndefined ? 0.0f
                                                   : YGResolveValue(trailingPosition, axisSize);
}

static float YGNodeBoundAxisWithinMinAndMax(const YGNodeRef node,
                                            const YGFlexDirection axis,
                                            const float value,
                                            const float axisSize) {
  float min = YGUndefined;
  float max = YGUndefined;

  if (YGFlexDirectionIsColumn(axis)) {
    min = YGResolveValue(&node->style.minDimensions[YGDimensionHeight], axisSize);
    max = YGResolveValue(&node->style.maxDimensions[YGDimensionHeight], axisSize);
  } else if (YGFlexDirectionIsRow(axis)) {
    min = YGResolveValue(&node->style.minDimensions[YGDimensionWidth], axisSize);
    max = YGResolveValue(&node->style.maxDimensions[YGDimensionWidth], axisSize);
  }

  float boundValue = value;

  if (!YGFloatIsUndefined(max) && max >= 0.0f && boundValue > max) {
    boundValue = max;
  }

  if (!YGFloatIsUndefined(min) && min >= 0.0f && boundValue < min) {
    boundValue = min;
  }

  return boundValue;
}

static inline YGValue *YGMarginLeadingValue(const YGNodeRef node, const YGFlexDirection axis) {
  if (YGFlexDirectionIsRow(axis) && node->style.margin[YGEdgeStart].unit != YGUnitUndefined) {
    return &node->style.margin[YGEdgeStart];
  } else {
    return &node->style.margin[leading[axis]];
  }
}

static inline YGValue *YGMarginTrailingValue(const YGNodeRef node, const YGFlexDirection axis) {
  if (YGFlexDirectionIsRow(axis) && node->style.margin[YGEdgeEnd].unit != YGUnitUndefined) {
    return &node->style.margin[YGEdgeEnd];
  } else {
    return &node->style.margin[trailing[axis]];
  }
}

// Like YGNodeBoundAxisWithinMinAndMax but also ensures that the value doesn't go
// below the
// padding and border amount.
// 用户修正 main 和 cross size
// 1. 不能小于 min 值和大于 max
// 2. 不能小于 border + padding 值
static inline float YGNodeBoundAxis(const YGNodeRef node,
                                    const YGFlexDirection axis,
                                    const float value,
                                    const float axisSize,
                                    const float widthSize) {
  return fmaxf(YGNodeBoundAxisWithinMinAndMax(node, axis, value, axisSize),
               YGNodePaddingAndBorderForAxis(node, axis, widthSize));
}

static void YGNodeSetChildTrailingPosition(const YGNodeRef node,
                                           const YGNodeRef child,
                                           const YGFlexDirection axis) {
  const float size = child->layout.measuredDimensions[dim[axis]];
  child->layout.position[trailing[axis]] =
      node->layout.measuredDimensions[dim[axis]] - size - child->layout.position[pos[axis]];
}

// If both left and right are defined, then use left. Otherwise return
// +left or -right depending on which is defined.
static float YGNodeRelativePosition(const YGNodeRef node,
                                    const YGFlexDirection axis,
                                    const float axisSize) {
  return YGNodeIsLeadingPosDefined(node, axis) ? YGNodeLeadingPosition(node, axis, axisSize)
                                               : -YGNodeTrailingPosition(node, axis, axisSize);
}

static void YGConstrainMaxSizeForMode(const YGNodeRef node,
                                      const enum YGFlexDirection axis,
                                      const float parentAxisSize,
                                      const float parentWidth,
                                      YGMeasureMode *mode,
                                      float *size) {
  const float maxSize = YGResolveValue(&node->style.maxDimensions[dim[axis]], parentAxisSize) +
                        YGNodeMarginForAxis(node, axis, parentWidth);
  switch (*mode) {
    case YGMeasureModeExactly:
    case YGMeasureModeAtMost:
      *size = (YGFloatIsUndefined(maxSize) || *size < maxSize) ? *size : maxSize;
      break;
    case YGMeasureModeUndefined:
      if (!YGFloatIsUndefined(maxSize)) {
        *mode = YGMeasureModeAtMost;
        *size = maxSize;
      }
      break;
  }
}

static void YGNodeSetPosition(const YGNodeRef node,
                              const YGDirection direction,
                              const float mainSize,
                              const float crossSize,
                              const float parentWidth) {
  /* Root nodes should be always layouted as LTR, so we don't return negative values. */
  const YGDirection directionRespectingRoot = node->parent != NULL ? direction : YGDirectionLTR;
  const YGFlexDirection mainAxis =
      YGResolveFlexDirection(node->style.flexDirection, directionRespectingRoot);
  const YGFlexDirection crossAxis = YGFlexDirectionCross(mainAxis, directionRespectingRoot);

  // 从 style.position 中取 leading 方向的值，如果定义了则为该值，否则尝试取 trailing 方向的值
  // 如果存在则取其负数，如果都没有定义则为 0
  const float relativePositionMain = YGNodeRelativePosition(node, mainAxis, mainSize);
  const float relativePositionCross = YGNodeRelativePosition(node, crossAxis, crossSize);

  node->layout.position[leading[mainAxis]] =
      YGNodeLeadingMargin(node, mainAxis, parentWidth) + relativePositionMain;
  node->layout.position[trailing[mainAxis]] =
      YGNodeTrailingMargin(node, mainAxis, parentWidth) + relativePositionMain;
  node->layout.position[leading[crossAxis]] =
      YGNodeLeadingMargin(node, crossAxis, parentWidth) + relativePositionCross;
  node->layout.position[trailing[crossAxis]] =
      YGNodeTrailingMargin(node, crossAxis, parentWidth) + relativePositionCross;
}

/*
 * 计算 computedFlexBasis，其中不包括 padding 和 border，但最小值为 padding + border
 * 这里也分两类：
 * 1. 不需要做内容 layout 就可以确定 flex basis: style 中的 flex basis 有值或者有固定的高宽
 * 2. 对 child 进行 layout，把 measureDimension 的宽度或者高度当做 flex basis
 */
static void YGNodeComputeFlexBasisForChild(const YGNodeRef node,
                                           const YGNodeRef child,
                                           const float width,
                                           const YGMeasureMode widthMode,
                                           const float height,
                                           const float parentWidth,
                                           const float parentHeight,
                                           const YGMeasureMode heightMode,
                                           const YGDirection direction,
                                           const YGConfigRef config) {
  const YGFlexDirection mainAxis = YGResolveFlexDirection(node->style.flexDirection, direction);
  const bool isMainAxisRow = YGFlexDirectionIsRow(mainAxis);
  const float mainAxisSize = isMainAxisRow ? width : height;
  const float mainAxisParentSize = isMainAxisRow ? parentWidth : parentHeight;

  float childWidth;
  float childHeight;
  YGMeasureMode childWidthMeasureMode;
  YGMeasureMode childHeightMeasureMode;

  const float resolvedFlexBasis =
      YGResolveValue(YGNodeResolveFlexBasisPtr(child), mainAxisParentSize);
  const bool isRowStyleDimDefined = YGNodeIsStyleDimDefined(child, YGFlexDirectionRow, parentWidth);
  const bool isColumnStyleDimDefined =
      YGNodeIsStyleDimDefined(child, YGFlexDirectionColumn, parentHeight);

  // 如果已知主轴 size，则考虑 style 中的 flex basis 设置，如果有值则取该值（不小于 padding + border）
  // ?:为何主轴 size 必须是已知的：因为 style 中的 flexbasis 只适用于伸缩计算，如果 mainsize 没有定义那么
  // 就无法做伸缩计算，flexbasis也会被忽略掉
  if (!YGFloatIsUndefined(resolvedFlexBasis) && !YGFloatIsUndefined(mainAxisSize)) {
    if (YGFloatIsUndefined(child->layout.computedFlexBasis) ||
        (YGConfigIsExperimentalFeatureEnabled(child->config, YGExperimentalFeatureWebFlexBasis) &&
        // gCurrentGenerationCount 只有在外部接口 YGNodeCalculateLayout 被调用时才会 +1
        // 这里的含义是指在每一轮 layout 过程中，computedFlexBasisGeneration 只计算一次
        // (存在一轮 layout 中多次执行这里的情况)
         child->layout.computedFlexBasisGeneration != gCurrentGenerationCount)) {
      child->layout.computedFlexBasis =
          fmaxf(resolvedFlexBasis, YGNodePaddingAndBorderForAxis(child, mainAxis, parentWidth));
    }
  } else if (isMainAxisRow && isRowStyleDimDefined) {
    // The width is definite, so use that as the flex basis.
    child->layout.computedFlexBasis =
        fmaxf(YGResolveValue(child->resolvedDimensions[YGDimensionWidth], parentWidth),
              YGNodePaddingAndBorderForAxis(child, YGFlexDirectionRow, parentWidth));
  } else if (!isMainAxisRow && isColumnStyleDimDefined) {
    // The height is definite, so use that as the flex basis.
    child->layout.computedFlexBasis =
        fmaxf(YGResolveValue(child->resolvedDimensions[YGDimensionHeight], parentHeight),
              YGNodePaddingAndBorderForAxis(child, YGFlexDirectionColumn, parentWidth));
  } else {
    // Compute the flex basis and hypothetical main size (i.e. the clamped
    // flex basis).
    /*
     * 以下是必须对内容进行布局才能计算出来 flex basis
     */
    childWidth = YGUndefined;
    childHeight = YGUndefined;
    childWidthMeasureMode = YGMeasureModeUndefined;
    childHeightMeasureMode = YGMeasureModeUndefined;

    const float marginRow = YGNodeMarginForAxis(child, YGFlexDirectionRow, parentWidth);
    const float marginColumn = YGNodeMarginForAxis(child, YGFlexDirectionColumn, parentWidth);

    // 计算布局需要的 availableWidth 和 availableHeight
    // 和外层第 2 个 if 条件结合可知，这里是指按列布局并且 style 中设置了宽度的情况
    if (isRowStyleDimDefined) {
      childWidth =
          YGResolveValue(child->resolvedDimensions[YGDimensionWidth], parentWidth) + marginRow;
      childWidthMeasureMode = YGMeasureModeExactly;
    }
    // 和外层第 3 个 if 条件结合可知，这里是指按行布局并且 style 中设置了高度的情况
    if (isColumnStyleDimDefined) {
      childHeight =
          YGResolveValue(child->resolvedDimensions[YGDimensionHeight], parentHeight) + marginColumn;
      childHeightMeasureMode = YGMeasureModeExactly;
    }

    // The W3C spec doesn't say anything about the 'overflow' property,
    // but all major browsers appear to implement the following logic.
    // 如果容器 overflow 非 scroll 或者至少 width 方向非 scroll，
    // 则 child 的 availableWidth 为容器宽度(前提是容器宽度是确定的)
    // 因为如果 width 方向为 scroll 的，则 availableWidth 应该为 undefined，完全按内容计算最终宽度
    if ((!isMainAxisRow && node->style.overflow == YGOverflowScroll) ||
        node->style.overflow != YGOverflowScroll) {
      if (YGFloatIsUndefined(childWidth) && !YGFloatIsUndefined(width)) {
        childWidth = width;
        childWidthMeasureMode = YGMeasureModeAtMost;
      }
    }

    if ((isMainAxisRow && node->style.overflow == YGOverflowScroll) ||
        node->style.overflow != YGOverflowScroll) {
      if (YGFloatIsUndefined(childHeight) && !YGFloatIsUndefined(height)) {
        childHeight = height;
        childHeightMeasureMode = YGMeasureModeAtMost;
      }
    }

    // 如果设置了高宽比则尝试计算完整的高度+宽度
    // ?: 如果这里通过 aspectRatio 已经拿到了主轴方向的 dimension，为何不直接以此为 flex basis
    // 而是再对 child 进行 layout，莫非 layout 之后 dimension 还会变?
    // 看 layout 中需要关注最后的 measuredDimension 和 avaialbeWidth 和 height 的关系
    if (!YGFloatIsUndefined(child->style.aspectRatio)) {
      if (!isMainAxisRow && childWidthMeasureMode == YGMeasureModeExactly) {
        childHeight = (childWidth - marginRow) / child->style.aspectRatio;
        childHeightMeasureMode = YGMeasureModeExactly;
      } else if (isMainAxisRow && childHeightMeasureMode == YGMeasureModeExactly) {
        childWidth = (childHeight - marginColumn) * child->style.aspectRatio;
        childWidthMeasureMode = YGMeasureModeExactly;
      }
    }

    // If child has no defined size in the cross axis and is set to stretch,
    // set the cross
    // axis to be measured exactly with the available inner width

    const bool hasExactWidth = !YGFloatIsUndefined(width) && widthMode == YGMeasureModeExactly;
    const bool childWidthStretch = YGNodeAlignItem(node, child) == YGAlignStretch &&
                                   childWidthMeasureMode != YGMeasureModeExactly; // 这里屏蔽掉已经通过 aspectRatio 计算出来的情况
    if (!isMainAxisRow && !isRowStyleDimDefined && hasExactWidth && childWidthStretch) {
      childWidth = width;
      childWidthMeasureMode = YGMeasureModeExactly;
      if (!YGFloatIsUndefined(child->style.aspectRatio)) {
        childHeight = (childWidth - marginRow) / child->style.aspectRatio;
        childHeightMeasureMode = YGMeasureModeExactly;
      }
    }

    const bool hasExactHeight = !YGFloatIsUndefined(height) && heightMode == YGMeasureModeExactly;
    const bool childHeightStretch = YGNodeAlignItem(node, child) == YGAlignStretch &&
                                    childHeightMeasureMode != YGMeasureModeExactly;
    if (isMainAxisRow && !isColumnStyleDimDefined && hasExactHeight && childHeightStretch) {
      childHeight = height;
      childHeightMeasureMode = YGMeasureModeExactly;

      if (!YGFloatIsUndefined(child->style.aspectRatio)) {
        childWidth = (childHeight - marginColumn) * child->style.aspectRatio;
        childWidthMeasureMode = YGMeasureModeExactly;
      }
    }

    YGConstrainMaxSizeForMode(
        child, YGFlexDirectionRow, parentWidth, parentWidth, &childWidthMeasureMode, &childWidth);
    YGConstrainMaxSizeForMode(child,
                              YGFlexDirectionColumn,
                              parentHeight,
                              parentWidth,
                              &childHeightMeasureMode,
                              &childHeight);

    // Measure the child
    YGLayoutNodeInternal(child,
                         childWidth,
                         childHeight,
                         direction,
                         childWidthMeasureMode,
                         childHeightMeasureMode,
                         parentWidth,
                         parentHeight,
                         false,
                         "measure",
                         config);

    // 整体 layout 之后，拿 dimension 作为 flex basis
    child->layout.computedFlexBasis =
        fmaxf(child->layout.measuredDimensions[dim[mainAxis]],
              YGNodePaddingAndBorderForAxis(child, mainAxis, parentWidth));
  }


  child->layout.computedFlexBasisGeneration = gCurrentGenerationCount;
}

static void YGNodeAbsoluteLayoutChild(const YGNodeRef node,
                                      const YGNodeRef child,
                                      const float width,
                                      const YGMeasureMode widthMode,
                                      const float height,
                                      const YGDirection direction,
                                      const YGConfigRef config) {
  const YGFlexDirection mainAxis = YGResolveFlexDirection(node->style.flexDirection, direction);
  const YGFlexDirection crossAxis = YGFlexDirectionCross(mainAxis, direction);
  const bool isMainAxisRow = YGFlexDirectionIsRow(mainAxis);

  float childWidth = YGUndefined;
  float childHeight = YGUndefined;
  YGMeasureMode childWidthMeasureMode = YGMeasureModeUndefined;
  YGMeasureMode childHeightMeasureMode = YGMeasureModeUndefined;

  const float marginRow = YGNodeMarginForAxis(child, YGFlexDirectionRow, width);
  const float marginColumn = YGNodeMarginForAxis(child, YGFlexDirectionColumn, width);

  if (YGNodeIsStyleDimDefined(child, YGFlexDirectionRow, width)) {
    childWidth = YGResolveValue(child->resolvedDimensions[YGDimensionWidth], width) + marginRow;
  } else {
    // If the child doesn't have a specified width, compute the width based
    // on the left/right
    // offsets if they're defined.
    if (YGNodeIsLeadingPosDefined(child, YGFlexDirectionRow) &&
        YGNodeIsTrailingPosDefined(child, YGFlexDirectionRow)) {
      childWidth = node->layout.measuredDimensions[YGDimensionWidth] -
                   (YGNodeLeadingBorder(node, YGFlexDirectionRow) +
                    YGNodeTrailingBorder(node, YGFlexDirectionRow)) -
                   (YGNodeLeadingPosition(child, YGFlexDirectionRow, width) +
                    YGNodeTrailingPosition(child, YGFlexDirectionRow, width));
      childWidth = YGNodeBoundAxis(child, YGFlexDirectionRow, childWidth, width, width);
    }
  }

  if (YGNodeIsStyleDimDefined(child, YGFlexDirectionColumn, height)) {
    childHeight =
        YGResolveValue(child->resolvedDimensions[YGDimensionHeight], height) + marginColumn;
  } else {
    // If the child doesn't have a specified height, compute the height
    // based on the top/bottom
    // offsets if they're defined.
    if (YGNodeIsLeadingPosDefined(child, YGFlexDirectionColumn) &&
        YGNodeIsTrailingPosDefined(child, YGFlexDirectionColumn)) {
      childHeight = node->layout.measuredDimensions[YGDimensionHeight] -
                    (YGNodeLeadingBorder(node, YGFlexDirectionColumn) +
                     YGNodeTrailingBorder(node, YGFlexDirectionColumn)) -
                    (YGNodeLeadingPosition(child, YGFlexDirectionColumn, height) +
                     YGNodeTrailingPosition(child, YGFlexDirectionColumn, height));
      childHeight = YGNodeBoundAxis(child, YGFlexDirectionColumn, childHeight, height, width);
    }
  }

  // Exactly one dimension needs to be defined for us to be able to do aspect ratio
  // calculation. One dimension being the anchor and the other being flexible.
  if (YGFloatIsUndefined(childWidth) ^ YGFloatIsUndefined(childHeight)) {
    if (!YGFloatIsUndefined(child->style.aspectRatio)) {
      if (YGFloatIsUndefined(childWidth)) {
        childWidth = marginRow + (childHeight - marginColumn) * child->style.aspectRatio;
      } else if (YGFloatIsUndefined(childHeight)) {
        childHeight = marginColumn + (childWidth - marginRow) / child->style.aspectRatio;
      }
    }
  }

  // If we're still missing one or the other dimension, measure the content.
  if (YGFloatIsUndefined(childWidth) || YGFloatIsUndefined(childHeight)) {
    childWidthMeasureMode =
        YGFloatIsUndefined(childWidth) ? YGMeasureModeUndefined : YGMeasureModeExactly;
    childHeightMeasureMode =
        YGFloatIsUndefined(childHeight) ? YGMeasureModeUndefined : YGMeasureModeExactly;

    // If the size of the parent is defined then try to constrain the absolute child to that size
    // as well. This allows text within the absolute child to wrap to the size of its parent.
    // This is the same behavior as many browsers implement.
    if (!isMainAxisRow && YGFloatIsUndefined(childWidth) && widthMode != YGMeasureModeUndefined &&
        width > 0) {
      childWidth = width;
      childWidthMeasureMode = YGMeasureModeAtMost;
    }

    YGLayoutNodeInternal(child,
                         childWidth,
                         childHeight,
                         direction,
                         childWidthMeasureMode,
                         childHeightMeasureMode,
                         childWidth,
                         childHeight,
                         false,
                         "abs-measure",
                         config);
    childWidth = child->layout.measuredDimensions[YGDimensionWidth] +
                 YGNodeMarginForAxis(child, YGFlexDirectionRow, width);
    childHeight = child->layout.measuredDimensions[YGDimensionHeight] +
                  YGNodeMarginForAxis(child, YGFlexDirectionColumn, width);
  }

  YGLayoutNodeInternal(child,
                       childWidth,
                       childHeight,
                       direction,
                       YGMeasureModeExactly,
                       YGMeasureModeExactly,
                       childWidth,
                       childHeight,
                       true,
                       "abs-layout",
                       config);

  if (YGNodeIsTrailingPosDefined(child, mainAxis) && !YGNodeIsLeadingPosDefined(child, mainAxis)) {
    child->layout.position[leading[mainAxis]] =
        node->layout.measuredDimensions[dim[mainAxis]] -
        child->layout.measuredDimensions[dim[mainAxis]] - YGNodeTrailingBorder(node, mainAxis) -
        YGNodeTrailingMargin(child, mainAxis, width) -
        YGNodeTrailingPosition(child, mainAxis, isMainAxisRow ? width : height);
  } else if (!YGNodeIsLeadingPosDefined(child, mainAxis) &&
             node->style.justifyContent == YGJustifyCenter) {
    child->layout.position[leading[mainAxis]] = (node->layout.measuredDimensions[dim[mainAxis]] -
                                                 child->layout.measuredDimensions[dim[mainAxis]]) /
                                                2.0f;
  } else if (!YGNodeIsLeadingPosDefined(child, mainAxis) &&
             node->style.justifyContent == YGJustifyFlexEnd) {
    child->layout.position[leading[mainAxis]] = (node->layout.measuredDimensions[dim[mainAxis]] -
                                                 child->layout.measuredDimensions[dim[mainAxis]]);
  }

  if (YGNodeIsTrailingPosDefined(child, crossAxis) &&
      !YGNodeIsLeadingPosDefined(child, crossAxis)) {
    child->layout.position[leading[crossAxis]] =
        node->layout.measuredDimensions[dim[crossAxis]] -
        child->layout.measuredDimensions[dim[crossAxis]] - YGNodeTrailingBorder(node, crossAxis) -
        YGNodeTrailingMargin(child, crossAxis, width) -
        YGNodeTrailingPosition(child, crossAxis, isMainAxisRow ? height : width);
  } else if (!YGNodeIsLeadingPosDefined(child, crossAxis) &&
             YGNodeAlignItem(node, child) == YGAlignCenter) {
    child->layout.position[leading[crossAxis]] =
        (node->layout.measuredDimensions[dim[crossAxis]] -
         child->layout.measuredDimensions[dim[crossAxis]]) /
        2.0f;
  } else if (!YGNodeIsLeadingPosDefined(child, crossAxis) &&
             ((YGNodeAlignItem(node, child) == YGAlignFlexEnd) ^
              (node->style.flexWrap == YGWrapWrapReverse))) {
    child->layout.position[leading[crossAxis]] = (node->layout.measuredDimensions[dim[crossAxis]] -
                                                  child->layout.measuredDimensions[dim[crossAxis]]);
  }
}

static void YGNodeWithMeasureFuncSetMeasuredDimensions(const YGNodeRef node,
                                                       const float availableWidth,
                                                       const float availableHeight,
                                                       const YGMeasureMode widthMeasureMode,
                                                       const YGMeasureMode heightMeasureMode,
                                                       const float parentWidth,
                                                       const float parentHeight) {
  YGAssertWithNode(node, node->measure != NULL, "Expected node to have custom measure function");

  const float paddingAndBorderAxisRow =
      YGNodePaddingAndBorderForAxis(node, YGFlexDirectionRow, availableWidth);
  const float paddingAndBorderAxisColumn =
      YGNodePaddingAndBorderForAxis(node, YGFlexDirectionColumn, availableWidth);
  const float marginAxisRow = YGNodeMarginForAxis(node, YGFlexDirectionRow, availableWidth);
  const float marginAxisColumn = YGNodeMarginForAxis(node, YGFlexDirectionColumn, availableWidth);

  // We want to make sure we don't call measure with negative size
  const float innerWidth = YGFloatIsUndefined(availableWidth)
                               ? availableWidth
                               : fmaxf(0, availableWidth - marginAxisRow - paddingAndBorderAxisRow);
  const float innerHeight =
      YGFloatIsUndefined(availableHeight)
          ? availableHeight
          : fmaxf(0, availableHeight - marginAxisColumn - paddingAndBorderAxisColumn);

  if (widthMeasureMode == YGMeasureModeExactly && heightMeasureMode == YGMeasureModeExactly) {
    // Don't bother sizing the text if both dimensions are already defined.
    node->layout.measuredDimensions[YGDimensionWidth] = YGNodeBoundAxis(
        node, YGFlexDirectionRow, availableWidth - marginAxisRow, parentWidth, parentWidth);
    node->layout.measuredDimensions[YGDimensionHeight] = YGNodeBoundAxis(
        node, YGFlexDirectionColumn, availableHeight - marginAxisColumn, parentHeight, parentWidth);
  } else {
    // Measure the text under the current constraints.
    const YGSize measuredSize =
        node->measure(node, innerWidth, widthMeasureMode, innerHeight, heightMeasureMode);

    node->layout.measuredDimensions[YGDimensionWidth] =
        YGNodeBoundAxis(node,
                        YGFlexDirectionRow,
                        (widthMeasureMode == YGMeasureModeUndefined ||
                         widthMeasureMode == YGMeasureModeAtMost)
                            ? measuredSize.width + paddingAndBorderAxisRow
                            : availableWidth - marginAxisRow,
                        availableWidth,
                        availableWidth);
    node->layout.measuredDimensions[YGDimensionHeight] =
        YGNodeBoundAxis(node,
                        YGFlexDirectionColumn,
                        (heightMeasureMode == YGMeasureModeUndefined ||
                         heightMeasureMode == YGMeasureModeAtMost)
                            ? measuredSize.height + paddingAndBorderAxisColumn
                            : availableHeight - marginAxisColumn,
                        availableHeight,
                        availableWidth);
  }
}

// For nodes with no children, use the available values if they were provided,
// or the minimum size as indicated by the padding and border sizes.
static void YGNodeEmptyContainerSetMeasuredDimensions(const YGNodeRef node,
                                                      const float availableWidth,
                                                      const float availableHeight,
                                                      const YGMeasureMode widthMeasureMode,
                                                      const YGMeasureMode heightMeasureMode,
                                                      const float parentWidth,
                                                      const float parentHeight) {
  const float paddingAndBorderAxisRow =
      YGNodePaddingAndBorderForAxis(node, YGFlexDirectionRow, parentWidth);
  const float paddingAndBorderAxisColumn =
      YGNodePaddingAndBorderForAxis(node, YGFlexDirectionColumn, parentWidth);
  const float marginAxisRow = YGNodeMarginForAxis(node, YGFlexDirectionRow, parentWidth);
  const float marginAxisColumn = YGNodeMarginForAxis(node, YGFlexDirectionColumn, parentWidth);

  node->layout.measuredDimensions[YGDimensionWidth] =
      YGNodeBoundAxis(node,
                      YGFlexDirectionRow,
                      (widthMeasureMode == YGMeasureModeUndefined ||
                       widthMeasureMode == YGMeasureModeAtMost)
                          ? paddingAndBorderAxisRow
                          : availableWidth - marginAxisRow,
                      parentWidth,
                      parentWidth);
  node->layout.measuredDimensions[YGDimensionHeight] =
      YGNodeBoundAxis(node,
                      YGFlexDirectionColumn,
                      (heightMeasureMode == YGMeasureModeUndefined ||
                       heightMeasureMode == YGMeasureModeAtMost)
                          ? paddingAndBorderAxisColumn
                          : availableHeight - marginAxisColumn,
                      parentHeight,
                      parentWidth);
}

// 判断 node 的 dimension 是否是确定的（即是否不需要做内容布局就可以计算出 dimension）
// 确定 的 dimension 就返回 true (同时设置measuredDimension) 否则返回 false
static bool YGNodeFixedSizeSetMeasuredDimensions(const YGNodeRef node,
                                                 const float availableWidth,
                                                 const float availableHeight,
                                                 const YGMeasureMode widthMeasureMode,
                                                 const YGMeasureMode heightMeasureMode,
                                                 const float parentWidth,
                                                 const float parentHeight) {
  // 这里列了两类、三种情况
  // 第一类，可用空间小于等于 0 (宽度或者高度)
  // 第二类，按 fill-available 布局
  // 这两类的特点是不需要布局内容就可以确定 dimension
  // 第一类，因为可用空间小于等于 0，因此内容的 dimension 直接就是 0，再加上 padding border 就得到最后的 dimension
  // 第二类 则直接按可用空间减去 margin 计算出 dimension
  if ((widthMeasureMode == YGMeasureModeAtMost && availableWidth <= 0.0f) ||
      (heightMeasureMode == YGMeasureModeAtMost && availableHeight <= 0.0f) ||
      (widthMeasureMode == YGMeasureModeExactly && heightMeasureMode == YGMeasureModeExactly)) {
    const float marginAxisColumn = YGNodeMarginForAxis(node, YGFlexDirectionColumn, parentWidth);
    const float marginAxisRow = YGNodeMarginForAxis(node, YGFlexDirectionRow, parentWidth);

    node->layout.measuredDimensions[YGDimensionWidth] =
        YGNodeBoundAxis(node,
                        YGFlexDirectionRow,
                        YGFloatIsUndefined(availableWidth) ||
                                (widthMeasureMode == YGMeasureModeAtMost && availableWidth < 0.0f)
                            ? 0.0f
                            : availableWidth - marginAxisRow,
                        parentWidth,
                        parentWidth);

    node->layout.measuredDimensions[YGDimensionHeight] =
        YGNodeBoundAxis(node,
                        YGFlexDirectionColumn,
                        YGFloatIsUndefined(availableHeight) ||
                                (heightMeasureMode == YGMeasureModeAtMost && availableHeight < 0.0f)
                            ? 0.0f
                            : availableHeight - marginAxisColumn,
                        parentHeight,
                        parentWidth);

    return true;
  }

  return false;
}

static void YGZeroOutLayoutRecursivly(const YGNodeRef node) {
  memset(&(node->layout), 0, sizeof(YGLayout));
  node->hasNewLayout = true;
  const uint32_t childCount = YGNodeGetChildCount(node);
  for (uint32_t i = 0; i < childCount; i++) {
    const YGNodeRef child = YGNodeListGet(node->children, i);
    YGZeroOutLayoutRecursivly(child);
  }
}

//
// This is the main routine that implements a subset of the flexbox layout
// algorithm
// described in the W3C YG documentation: https://www.w3.org/TR/YG3-flexbox/.
//
// Limitations of this algorithm, compared to the full standard:
//  * Display property is always assumed to be 'flex' except for Text nodes,
//  which
//    are assumed to be 'inline-flex'.
//  * The 'zIndex' property (or any form of z ordering) is not supported. Nodes
//  are
//    stacked in document order.
//  * The 'order' property is not supported. The order of flex items is always
//  defined
//    by document order.
//  * The 'visibility' property is always assumed to be 'visible'. Values of
//  'collapse'
//    and 'hidden' are not supported.
//  * There is no support for forced breaks.
//  * It does not support vertical inline directions (top-to-bottom or
//  bottom-to-top text).
//
// Deviations from standard:
//  * Section 4.5 of the spec indicates that all flex items have a default
//  minimum
//    main size. For text blocks, for example, this is the width of the widest
//    word.
//    Calculating the minimum width is expensive, so we forego it and assume a
//    default
//    minimum main size of 0.
//  * Min/Max sizes in the main axis are not honored when resolving flexible
//  lengths.
//  * The spec indicates that the default value for 'flexDirection' is 'row',
//  but
//    the algorithm below assumes a default of 'column'.
//
// Input parameters:
//    - node: current node to be sized and layed out
//    - availableWidth & availableHeight: available size to be used for sizing
//    the node
//      or YGUndefined if the size is not available; interpretation depends on
//      layout
//      flags
//    - parentDirection: the inline (text) direction within the parent
//    (left-to-right or
//      right-to-left)
//    - widthMeasureMode: indicates the sizing rules for the width (see below
//    for explanation)
//    - heightMeasureMode: indicates the sizing rules for the height (see below
//    for explanation)
//    - performLayout: specifies whether the caller is interested in just the
//    dimensions
//      of the node or it requires the entire node and its subtree to be layed
//      out
//      (with final positions)
//
// Details:
//    This routine is called recursively to lay out subtrees of flexbox
//    elements. It uses the
//    information in node.style, which is treated as a read-only input. It is
//    responsible for
//    setting the layout.direction and layout.measuredDimensions fields for the
//    input node as well
//    as the layout.position and layout.lineIndex fields for its child nodes.
//    The
//    layout.measuredDimensions field includes any border or padding for the
//    node but does
//    not include margins.
//
//    The spec describes four different layout modes: "fill available", "max
//    content", "min
//    content",
//    and "fit content". Of these, we don't use "min content" because we don't
//    support default
//    minimum main sizes (see above for details). Each of our measure modes maps
//    to a layout mode
//    from the spec (https://www.w3.org/TR/YG3-sizing/#terms):
//      - YGMeasureModeUndefined: max content
//      - YGMeasureModeExactly: fill available
//      - YGMeasureModeAtMost: fit content
//
//    When calling YGNodelayoutImpl and YGLayoutNodeInternal, if the caller passes
//    an available size of
//    undefined then it must also pass a measure mode of YGMeasureModeUndefined
//    in that dimension.
//    如果 available size 为 undefined，则对应的 measure mode 必须是 max-content
//    因为 fit-content 和 fill-available 都需要依赖可用空间，因此
//    如果可用空间为 undefined 则只支持 max-content，即按文本不换行计算内容宽度）

//    YGMeasureModeUndefined: 主轴 dimension 由所有 flex basis 之和确定；
//    YGMeasureModeAtMost: 主轴 dimension 在 flex basis 之和与 available main size 之间取最小值，如果取后者还需要对孩子进行伸缩布局
//    YGMeasureModeExactly: 主轴 dimension 即为 available main size

//    max-content vs fit-content
//    1. max-content 不考虑可用空间，只计算最大内容宽度，如果是文本则计算不换行情况下的宽度
//    2. fit-content 在 max-content 基础上会考虑可用空间，不超过可用空间
//    3. 两者都需要对内容进行布局
//    4. 只有 fill available 是不需要布局就能确定宽度的
static void YGNodelayoutImpl(const YGNodeRef node,
                             const float availableWidth,
                             const float availableHeight,
                             const YGDirection parentDirection,
                             const YGMeasureMode widthMeasureMode,
                             const YGMeasureMode heightMeasureMode,
                             const float parentWidth, // 用于计算百分比值的实际值
                             const float parentHeight,
                             const bool performLayout,
                             const YGConfigRef config) {
  YGAssertWithNode(node,
                   YGFloatIsUndefined(availableWidth) ? widthMeasureMode == YGMeasureModeUndefined
                                                      : true,
                   "availableWidth is indefinite so widthMeasureMode must be "
                   "YGMeasureModeUndefined");
  YGAssertWithNode(node,
                   YGFloatIsUndefined(availableHeight) ? heightMeasureMode == YGMeasureModeUndefined
                                                       : true,
                   "availableHeight is indefinite so heightMeasureMode must be "
                   "YGMeasureModeUndefined");

  // Set the resolved resolution in the node's layout.
  const YGDirection direction = YGNodeResolveDirection(node, parentDirection);
  // 文本方向: LTR or RTL
  node->layout.direction = direction;

  // 基于 direction 判断时 row 或者 row-reverse（LTR 和 RTL 正好相反）
  const YGFlexDirection flexRowDirection = YGResolveFlexDirection(YGFlexDirectionRow, direction);
  const YGFlexDirection flexColumnDirection =
      YGResolveFlexDirection(YGFlexDirectionColumn, direction);

  // leading 的概念表示某个轴的起始布局方向，根据 direction 和轴的不同，可以是上下左右
  // 但以下的 start 和 end 的概念仅用于水平方向，左或者右
  node->layout.margin[YGEdgeStart] = YGNodeLeadingMargin(node, flexRowDirection, parentWidth);
  node->layout.margin[YGEdgeEnd] = YGNodeTrailingMargin(node, flexRowDirection, parentWidth);
  node->layout.margin[YGEdgeTop] = YGNodeLeadingMargin(node, flexColumnDirection, parentWidth);
  node->layout.margin[YGEdgeBottom] = YGNodeTrailingMargin(node, flexColumnDirection, parentWidth);

  node->layout.border[YGEdgeStart] = YGNodeLeadingBorder(node, flexRowDirection);
  node->layout.border[YGEdgeEnd] = YGNodeTrailingBorder(node, flexRowDirection);
  node->layout.border[YGEdgeTop] = YGNodeLeadingBorder(node, flexColumnDirection);
  node->layout.border[YGEdgeBottom] = YGNodeTrailingBorder(node, flexColumnDirection);

  node->layout.padding[YGEdgeStart] = YGNodeLeadingPadding(node, flexRowDirection, parentWidth);
  node->layout.padding[YGEdgeEnd] = YGNodeTrailingPadding(node, flexRowDirection, parentWidth);
  node->layout.padding[YGEdgeTop] = YGNodeLeadingPadding(node, flexColumnDirection, parentWidth);
  node->layout.padding[YGEdgeBottom] =
      YGNodeTrailingPadding(node, flexColumnDirection, parentWidth);

  // 如果节点有 measure 方法（文本节点）则直接计算 dimension 并且不需要继续往后处理
  // 此处也是递归布局的结束条件之一
  if (node->measure) {
    // 计算文本节点的 dimensioin
    // 1. 如果 measure mode 是 max-content 或 fit-content 则需要调用 measure 方法计算 dimension
    // 然后再加上padding 和 border 就是最终的 dimension（按宽度和高度分别计算）
    // 2. 如果宽、高 measure mode 都是 fill-available（此时 availableWidth 和 availableHeight
    // 必须不为 undefined），则并不会调用 measure 方法，而是直接用可用空间减去相应的 margin 得到 dimension
    YGNodeWithMeasureFuncSetMeasuredDimensions(node,
                                               availableWidth,
                                               availableHeight,
                                               widthMeasureMode,
                                               heightMeasureMode,
                                               parentWidth,
                                               parentHeight);
    return;
  }

  const uint32_t childCount = YGNodeListCount(node->children);
  // 如果节点没有孩子节点，那么计算过程变得更加简单，退出递归条件之二
  // 1. 如果是 measure mode 是 fill-available 则同上
  // 2. 如果不是则将内容 dimension 设置为 0，以此加上 padding border 得到最后的 dimension
  if (childCount == 0) {
    YGNodeEmptyContainerSetMeasuredDimensions(node,
                                              availableWidth,
                                              availableHeight,
                                              widthMeasureMode,
                                              heightMeasureMode,
                                              parentWidth,
                                              parentHeight);
    return;
  }

  // If we're not being asked to perform a full layout we can skip the algorithm if we already know
  // the size
  // 如果不要求做深度布局并且当前的 node 的 dimension 计算不依赖其内容则直接计算并返回(如果需要依赖内容才能
  // 算出 dimension 就必须得继续递归)，退出递归条件之三
  if (!performLayout && YGNodeFixedSizeSetMeasuredDimensions(node,
                                                             availableWidth,
                                                             availableHeight,
                                                             widthMeasureMode,
                                                             heightMeasureMode,
                                                             parentWidth,
                                                             parentHeight)) {
    return;
  }

  // 以上三种退出条件都是不需要做 layout 就能确定 dimension 的情况，从这里布局才正式开始
  // Reset layout flags, as they could have changed.
  node->layout.hadOverflow = false;

  // STEP 1: CALCULATE VALUES FOR REMAINDER OF ALGORITHM
  /*
   * 步骤1（和规范中的算法步骤1不对应）：预备布局需要的 value
   * 1. 主轴和交叉轴的值(row row-reverse column column-reverse)
   * 2. 水平、垂直方向的盒模型值：padding border margin
   */
  const YGFlexDirection mainAxis = YGResolveFlexDirection(node->style.flexDirection, direction);
  const YGFlexDirection crossAxis = YGFlexDirectionCross(mainAxis, direction);
  const bool isMainAxisRow = YGFlexDirectionIsRow(mainAxis);
  const YGJustify justifyContent = node->style.justifyContent;
  const bool isNodeFlexWrap = node->style.flexWrap != YGWrapNoWrap;

  const float mainAxisParentSize = isMainAxisRow ? parentWidth : parentHeight;
  const float crossAxisParentSize = isMainAxisRow ? parentHeight : parentWidth;

  YGNodeRef firstAbsoluteChild = NULL;
  YGNodeRef currentAbsoluteChild = NULL;

  const float leadingPaddingAndBorderMain =
      YGNodeLeadingPaddingAndBorder(node, mainAxis, parentWidth);
  const float trailingPaddingAndBorderMain =
      YGNodeTrailingPaddingAndBorder(node, mainAxis, parentWidth);
  const float leadingPaddingAndBorderCross =
      YGNodeLeadingPaddingAndBorder(node, crossAxis, parentWidth);
  const float paddingAndBorderAxisMain = YGNodePaddingAndBorderForAxis(node, mainAxis, parentWidth);
  const float paddingAndBorderAxisCross =
      YGNodePaddingAndBorderForAxis(node, crossAxis, parentWidth);

  YGMeasureMode measureModeMainDim = isMainAxisRow ? widthMeasureMode : heightMeasureMode;
  YGMeasureMode measureModeCrossDim = isMainAxisRow ? heightMeasureMode : widthMeasureMode;

  const float paddingAndBorderAxisRow =
      isMainAxisRow ? paddingAndBorderAxisMain : paddingAndBorderAxisCross;
  const float paddingAndBorderAxisColumn =
      isMainAxisRow ? paddingAndBorderAxisCross : paddingAndBorderAxisMain;

  const float marginAxisRow = YGNodeMarginForAxis(node, YGFlexDirectionRow, parentWidth);
  const float marginAxisColumn = YGNodeMarginForAxis(node, YGFlexDirectionColumn, parentWidth);

  // STEP 2: DETERMINE AVAILABLE SIZE IN MAIN AND CROSS DIRECTIONS
  /*
   * 步骤 2：计算各个轴的可用空间（提供给孩子节点进行伸缩布局的内容 dimension，即 node 的 content-box 宽度高度)
   */

  // 根据 style 中设置的最大/最小宽度和高度，计算可用的最大最小内容宽高
  const float minInnerWidth =
      YGResolveValue(&node->style.minDimensions[YGDimensionWidth], parentWidth) - marginAxisRow -
      paddingAndBorderAxisRow;
  const float maxInnerWidth =
      YGResolveValue(&node->style.maxDimensions[YGDimensionWidth], parentWidth) - marginAxisRow -
      paddingAndBorderAxisRow;
  const float minInnerHeight =
      YGResolveValue(&node->style.minDimensions[YGDimensionHeight], parentHeight) -
      marginAxisColumn - paddingAndBorderAxisColumn;
  const float maxInnerHeight =
      YGResolveValue(&node->style.maxDimensions[YGDimensionHeight], parentHeight) -
      marginAxisColumn - paddingAndBorderAxisColumn;
  const float minInnerMainDim = isMainAxisRow ? minInnerWidth : minInnerHeight;
  const float maxInnerMainDim = isMainAxisRow ? maxInnerWidth : maxInnerHeight;

  // Max dimension overrides predefined dimension value; Min dimension in turn overrides both of the
  // above
  float availableInnerWidth = availableWidth - marginAxisRow - paddingAndBorderAxisRow;
  // 如果可用的内容宽度可以根据 availableWidth 计算出来，则做一次边界矫正
  if (!YGFloatIsUndefined(availableInnerWidth)) {
    // We want to make sure our available width does not violate min and max constraints
    availableInnerWidth = fmaxf(fminf(availableInnerWidth, maxInnerWidth), minInnerWidth);
  }

  float availableInnerHeight = availableHeight - marginAxisColumn - paddingAndBorderAxisColumn;
  // 如果可用的内容高度可以根据 availableWidth 计算出来，则做一次边界矫正
  if (!YGFloatIsUndefined(availableInnerHeight)) {
    // We want to make sure our available height does not violate min and max constraints
    availableInnerHeight = fmaxf(fminf(availableInnerHeight, maxInnerHeight), minInnerHeight);
  }

  // 至此已经计算出来了可用于布局的 inner dimension，当然它可能是 undefined（需要先对内容做 layout）

  float availableInnerMainDim = isMainAxisRow ? availableInnerWidth : availableInnerHeight;
  const float availableInnerCrossDim = isMainAxisRow ? availableInnerHeight : availableInnerWidth;

  // If there is only one child with flexGrow + flexShrink it means we can set the
  // computedFlexBasis to 0 instead of measuring and shrinking / flexing the child to exactly
  // match the remaining space
  // 如果只有一个伸缩布局的孩子节点，则不需要考虑 flex basis 的值，因为所有弹性空间的计算上只需要考虑这一个节点
  YGNodeRef singleFlexChild = NULL;
  if (measureModeMainDim == YGMeasureModeExactly) {
    for (uint32_t i = 0; i < childCount; i++) {
      const YGNodeRef child = YGNodeGetChild(node, i);
      if (singleFlexChild) {
        if (YGNodeIsFlex(child)) {
          // There is already a flexible child, abort.
          singleFlexChild = NULL;
          break;
        }
      } else if (YGResolveFlexGrow(child) > 0.0f && YGNodeResolveFlexShrink(child) > 0.0f) {
        singleFlexChild = child;
      }
    }
  }

  // 所有参与弹性布局的孩子节点的 flex basis 之和 + margin 之和
  float totalOuterFlexBasis = 0;

  // STEP 3: DETERMINE FLEX BASIS FOR EACH ITEM
  // 步骤 3：计算每一个孩子节点的 flex basis
  for (uint32_t i = 0; i < childCount; i++) {
    const YGNodeRef child = YGNodeListGet(node->children, i);
    if (child->style.display == YGDisplayNone) {
      // 如果 display:none 则设置其 layout 为 0
      YGZeroOutLayoutRecursivly(child);
      child->hasNewLayout = true;
      child->isDirty = false;
      continue;
    }
    // 根据 style 中的 dimension 值，设置 resolvedDimension 中的值
    // 如果 style 中 max 和 min dimension 相等，则 resolvedDimension 直接为 max dimension
    // 否则就为 style 中的 dimension
    YGResolveDimensions(child);

    if (performLayout) {
      // Set the initial position (relative to the parent).
      const YGDirection childDirection = YGNodeResolveDirection(child, direction);
      // 根据 style.position 的值设置 layout.position 值
      YGNodeSetPosition(child,
                        childDirection,
                        availableInnerMainDim,
                        availableInnerCrossDim,
                        availableInnerWidth);
    }

    // Absolute-positioned children don't participate in flex layout. Add them
    // to a list that we can process later.
    // 把绝对定位的孩子节点单独放入链表（他们不参与 layout）
    if (child->style.positionType == YGPositionTypeAbsolute) {
      // Store a private linked list of absolutely positioned children
      // so that we can efficiently traverse them later.
      if (firstAbsoluteChild == NULL) {
        firstAbsoluteChild = child;
      }
      if (currentAbsoluteChild != NULL) {
        currentAbsoluteChild->nextChild = child;
      }
      currentAbsoluteChild = child;
      child->nextChild = NULL;
    } else {
      // 只有一个节点时不考虑 flex basis，将 computedFlexBasis 设置为 0
      if (child == singleFlexChild) {
        // 这个赋值意味着本轮 Layout 中其 computedFlexBasis 不需要再次计算了
        child->layout.computedFlexBasisGeneration = gCurrentGenerationCount;
        child->layout.computedFlexBasis = 0;
      } else {
        // 根据节点的宽高或者 layout 后的 dimension 确定 flex basis
        YGNodeComputeFlexBasisForChild(node,
                                       child,
                                       availableInnerWidth,
                                       widthMeasureMode,
                                       availableInnerHeight,
                                       availableInnerWidth,
                                       availableInnerHeight,
                                       heightMeasureMode,
                                       direction,
                                       config);
      }
    }

    // 累加所有 flex basis
    totalOuterFlexBasis +=
        child->layout.computedFlexBasis + YGNodeMarginForAxis(child, mainAxis, availableInnerWidth);
    ;
  }

  printf("totalOuterFlexBasis = %f \n", totalOuterFlexBasis);

  const bool flexBasisOverflows = measureModeMainDim == YGMeasureModeUndefined
                                      ? false // max-content 情形，内容多宽(高)，容器就多宽
                                      : totalOuterFlexBasis > availableInnerMainDim;
  // 如果 flex basis 超出可用空间则会进行换行处理，每一行的宽度是确定的（availableInnerMainDim）
  // 所以，measure mode 需要调整为 YGMeasureModeExactly
  // 当 totalOuterFlexBasis 计算出来之后，availableInnerMainDim 就可以确定了
  // 1. 如果初始 availableInnerMainDim 非 undefined，则保持原来的值
  // 2. 如果初始 availableInnerMainDim 为 undefined，则为 totalOuterFlexBasis
  // 3. 如果是 1 并且是多行模式，则每一行的宽度可以确定为 availableInnerMainDim，因此 measureModeMainDim 可以变更为 YGMeasureModeExactly
  if (isNodeFlexWrap && flexBasisOverflows && measureModeMainDim == YGMeasureModeAtMost) {
    measureModeMainDim = YGMeasureModeExactly;
  }

  // STEP 4: COLLECT FLEX ITEMS INTO FLEX LINES

  // Indexes of children that represent the first and last items in the line.
  uint32_t startOfLineIndex = 0;
  uint32_t endOfLineIndex = 0;

  // Number of lines.
  uint32_t lineCount = 0;

  // Accumulated cross dimensions of all lines so far.
  float totalLineCrossDim = 0;

  // Max main dimension of all the lines.
  float maxLineMainDim = 0;

  // 第一层大循环，每次循环结束时，endOfLineIndex 指向第一个在当前行放不下的节点（下一行的第一个节点）
  for (; endOfLineIndex < childCount; lineCount++, startOfLineIndex = endOfLineIndex) {
    // Number of items on the currently line. May be different than the
    // difference
    // between start and end indicates because we skip over absolute-positioned
    // items.
    uint32_t itemsOnLine = 0;

    // sizeConsumedOnCurrentLine is accumulation of the dimensions and margin
    // of all the children on the current line. This will be used in order to
    // either set the dimensions of the node if none already exist or to compute
    // the remaining space left for the flexible children.
    float sizeConsumedOnCurrentLine = 0;
    float sizeConsumedOnCurrentLineIncludingMinConstraint = 0;

    float totalFlexGrowFactors = 0;
    float totalFlexShrinkScaledFactors = 0;

    // Maintain a linked list of the child nodes that can shrink and/or grow.
    YGNodeRef firstRelativeChild = NULL;
    YGNodeRef currentRelativeChild = NULL;

    // Add items to the current line until it's full or we run out of items.
    for (uint32_t i = startOfLineIndex; i < childCount; i++, endOfLineIndex++) {
      const YGNodeRef child = YGNodeListGet(node->children, i);
      if (child->style.display == YGDisplayNone) {
        continue;
      }
      child->lineIndex = lineCount;

      if (child->style.positionType != YGPositionTypeAbsolute) {
        const float childMarginMainAxis = YGNodeMarginForAxis(child, mainAxis, availableInnerWidth);
        const float flexBasisWithMaxConstraints =
            fminf(YGResolveValue(&child->style.maxDimensions[dim[mainAxis]], mainAxisParentSize),
                  child->layout.computedFlexBasis);
        const float flexBasisWithMinAndMaxConstraints =
            fmaxf(YGResolveValue(&child->style.minDimensions[dim[mainAxis]], mainAxisParentSize),
                  flexBasisWithMaxConstraints);

        // If this is a multi-line flow and this item pushes us over the
        // available size, we've
        // hit the end of the current line. Break out of the loop and lay out
        // the current line.
        // 第一层循环退出条件：孩子节点 flex basis 和 margin 的累加宽度超过可用宽度
        if (sizeConsumedOnCurrentLineIncludingMinConstraint + flexBasisWithMinAndMaxConstraints +
                    childMarginMainAxis >
                availableInnerMainDim &&
            isNodeFlexWrap && itemsOnLine > 0) {
          break;
        }

        // ?: 为啥这里要设置两个相同的值
        sizeConsumedOnCurrentLineIncludingMinConstraint +=
            flexBasisWithMinAndMaxConstraints + childMarginMainAxis;
        // 后面逻辑有个判断 sizeConsumedOnCurrentLine < 0
        // ?: 从这里看只有 margin 小于 0 会导致 sizeConsumedOnCurrentLine 小于 0
        sizeConsumedOnCurrentLine += flexBasisWithMinAndMaxConstraints + childMarginMainAxis;
        itemsOnLine++;

        // 累加 grow 和 shrink factors
        if (YGNodeIsFlex(child)) {
          // grow factor 直接累加即可
          totalFlexGrowFactors += YGResolveFlexGrow(child);

          // Unlike the grow factor, the shrink factor is scaled relative to the child dimension.
          // shrink factor 需要乘以自身的 flex basis 后再累加
          // 这里用负数便于后面分配空间时统一处理（shrink 相当于加一个负空间）
          totalFlexShrinkScaledFactors +=
              -YGNodeResolveFlexShrink(child) * child->layout.computedFlexBasis;
        }

        // Store a private linked list of children that need to be layed out.
        if (firstRelativeChild == NULL) {
          firstRelativeChild = child;
        }
        if (currentRelativeChild != NULL) {
          currentRelativeChild->nextChild = child;
        }
        currentRelativeChild = child;
        child->nextChild = NULL;
      }
    }

    // The total flex factor needs to be floored to 1.
    // 这里对于累加 factors 小于 1 的情况和标准中不一致
    // 标准：对于累加 factors 小于 1 的情况，需要用累加 factors 乘以剩余空间得到的值作为新的剩余空间分配
    // yoga: 小于 1 的会设置为 1，也就是说用始终用 1 倍剩余空间分配
    if (totalFlexGrowFactors > 0 && totalFlexGrowFactors < 1) {
      totalFlexGrowFactors = 1;
    }

    // The total flex shrink factor needs to be floored to 1.
    if (totalFlexShrinkScaledFactors > 0 && totalFlexShrinkScaledFactors < 1) {
      totalFlexShrinkScaledFactors = 1;
    }

    // If we don't need to measure the cross axis, we can skip the entire flex
    // step.
    /*
     * 条件分析：
     * 1. layout 方法起始处有三种条件会退出递归布局，其中最后一种是 node 的宽度和高度都是确定的
     * 即宽度和高度的 measure mode 都必须是 YGMeasureModeExactly
     * 2. 因此能够进入到当前这一步必然是宽度或者高度至少有一个维度是非 YGMeasureModeExactly
     * 3. canSkipFlex 条件中设定交叉轴必须是 YGMeasureModeExactly，那么此时主轴必然是非 YGMeasureModeExactly
     * 即主轴必须通过内容来计算其 dimension（flex basis 之和计算出来之后，availableInnerMainDim 肯定是可以确定的）
     * 4. 主轴如果是非 YGMeasureModeExactly 则按照内容的宽度来计算最终的 dimesion
     * 5. 因此 canSkipFlex 的含义是：如果不需要做深度 layout，并且交叉轴的 dimension 已经是确定的，而且
     * 主轴的 dimension 按照内容宽度来确定则可以不需要对内容做弹性伸缩计算（直接基于孩子节点的 computedFlexBasis
     * 和 margin 来获得主轴方向的 dimension)
     * 6. 这种情况下的剩余空间将会是 0
     * 理解：
     * canSkipFlex 意味着不需要对子节点做伸缩计算，原因在于：
     * 1. !performLayout：即外部调用接口时已明确告知不需要做子节点 layout，只需要当前 node 的 dimension
     * 2. measureModeCrossDim == YGMeasureModeExactly：cross 轴方向的 dimension 已知
     * 3. 到这一步后，所有内容的宽度已经计算出来了而且 main 轴的 measure mode 一定是按内容计算（最多考虑可用空间）
     * 因此，main 轴方向的 dimension 也可以拿到了（这个值有可能小于 node 最小边界，后面会处理）
     */
    const bool canSkipFlex = !performLayout && measureModeCrossDim == YGMeasureModeExactly;

    // In order to position the elements in the main axis, we have two
    // controls. The space between the beginning and the first element
    // and the space between each two elements.
    float leadingMainDim = 0;
    float betweenMainDim = 0;

    // STEP 5: RESOLVING FLEXIBLE LENGTHS ON MAIN AXIS
    // Calculate the remaining available space that needs to be allocated.
    // If the main dimension size isn't known, it is computed based on
    // the line length, so there's no more space left to distribute.

    // If we don't measure with exact main dimension we want to ensure we don't violate min and max
    /* 推断：measureModeMainDim != YGMeasureModeExactly
     * 1. measureModeMainDim == YGMeasureModeAtMost
     * 按内容计算 dimension 同时参考可用空间，可能会多行
     * 2. measureModeMainDim == YGMeasureModeUndefined
     * 完全按内容计算 dimension，无换行情况
     */
    if (measureModeMainDim != YGMeasureModeExactly) {
      if (!YGFloatIsUndefined(minInnerMainDim) && sizeConsumedOnCurrentLine < minInnerMainDim) {
        availableInnerMainDim = minInnerMainDim;
      } else if (!YGFloatIsUndefined(maxInnerMainDim) &&
                 sizeConsumedOnCurrentLine > maxInnerMainDim) {
        availableInnerMainDim = maxInnerMainDim;
      } else {
        if (!node->config->useLegacyStretchBehaviour &&
          // totalFlexGrowFactors == 0: 这个条件是指当前这一行的所有子节点都不会伸张，（最大就是内容宽度）
          // 因为在多行的情况下，当前行可能会比其他行宽度小，因此存在可用空间动态调整的情况（所有行都确定后）
          // 一旦确定孩子节点都不能伸张当前行的可用空间也就可以确定了
          // YGResolveFlexGrow(node) == 0: 这个条件是指 node 本身不会弹性伸张，（最小就是内容宽度）
            (totalFlexGrowFactors == 0 || YGResolveFlexGrow(node) == 0)) {
          // If we don't have any children to flex or we can't flex the node itself,
          // space we've used is all space we need. Root node also should be shrunk to minimum
          // 基于内容计算 main dimension
          availableInnerMainDim = sizeConsumedOnCurrentLine;
        }
      }
    }

    /*
     * 计算剩余空间 - availableInnerMainDim 在非 YGMeasureModeExactly 模式下会基于内容和 min/max 进行计算
     * 1. 如果 availableInnerMainDim 是确定值，则剩余空间通过减法运算可得；
     * 2. 如果 availableInnerMainDim 不确定(indefinite)，即 main dim 依赖其内容，则剩余空间为 0
     * 3. ?: sizeConsumedOnCurrentLine < 0 什么情况会出现
     */
    float remainingFreeSpace = 0;
    if (!YGFloatIsUndefined(availableInnerMainDim)) {
      /*
       * 如果是基于内容宽度计算得出的 availableInnerMainDim 则 remainingFreeSpace 有三种情况：
       * 1. availableInnerMainDim 经过 min 修正，则 remainingFreeSpace > 0
       * 2. availableInnerMainDim 经过 max 修正，则 remainingFreeSpace < 0
       * 3. availableInnerMainDim 未修正，则 remainingFreeSpace = 0
       * 即在非 YGMeasureModeExactly 模式下，弹性伸缩的空间由 min/max 越界造成，若无越界则无需分配空间
       */
      remainingFreeSpace = availableInnerMainDim - sizeConsumedOnCurrentLine;
    // 当 margin 为负并且大于 flexbasis 之和时，sizeConsumedOnCurrentLine 会小于 0
    // 此时，remainingFreeSpace > 0 走 flexGrow 逻辑
    // https://codepen.io/millerisme/pen/ooqORg
    } else if (sizeConsumedOnCurrentLine < 0) {
      // availableInnerMainDim is indefinite which means the node is being sized based on its
      // content.
      // sizeConsumedOnCurrentLine is negative which means the node will allocate 0 points for
      // its content. Consequently, remainingFreeSpace is 0 - sizeConsumedOnCurrentLine.
      remainingFreeSpace = -sizeConsumedOnCurrentLine;
    }

    const float originalRemainingFreeSpace = remainingFreeSpace;
    float deltaFreeSpace = 0;

    if (!canSkipFlex) {
      float childFlexBasis;
      float flexShrinkScaledFactor;
      float flexGrowFactor;
      float baseMainSize;
      float boundMainSize;

      // Do two passes over the flex items to figure out how to distribute the
      // remaining space.
      // The first pass finds the items whose min/max constraints trigger,
      // freezes them at those
      // sizes, and excludes those sizes from the remaining space. The second
      // pass sets the size
      // of each flexible item. It distributes the remaining space amongst the
      // items whose min/max
      // constraints didn't trigger in pass 1. For the other items, it sets
      // their sizes by forcing
      // their min/max constraints to trigger again.
      //
      // This two pass approach for resolving min/max constraints deviates from
      // the spec. The
      // spec (https://www.w3.org/TR/YG-flexbox-1/#resolve-flexible-lengths)
      // describes a process
      // that needs to be repeated a variable number of times. The algorithm
      // implemented here
      // won't handle all cases but it was simpler to implement and it mitigates
      // performance
      // concerns because we know exactly how many passes it'll do.
      /*
       * 和标准推荐的算法不一样，这里只用了两次循环来计算伸缩 size，按标准每一次循环只能冻结一部分越界的节点
       * 下一次循环重新计算剩余空间后再冻结一部分，直到节点都冻结或者没有剩余空间。
       * Yoga 的算法进行了简化，只用了两次循环：
       * 1. 第一次循环
       * 按初始剩余空间计算所有孩子应该分配的空间，做一次边界矫正，得到分配给未越界孩子的总剩余空间，同时将
       * 越界孩子的伸缩因子从因子累加和中减去（剩余空间和累加因子都调整为只和未越界孩子相关）
       * 2. 第二次循环
       * 对于在第一次循环计算中未越界的孩子按正常比例分配剩余空间（新比例、新剩余空间）
       * 对于在第一次循环计算中已经越界的孩子节点，这里会按扩大的比例计算，按注释中的说法是为了让它强行越界，
       * 和第一次循环中的越界结果保持一致（类似于在第一次循环中冻结越界的节点，但这么做逻辑上有点诡异）
       * 第二次循环和标准算法的差异在于，这次循环中仍然会有一批新的越界节点产生，剩余空间会继续发生变化，按
       * 标准算法需要进行下一次循环来继续分配，但 yoga 的算法只做一次分配，因此可能在最终的空间分配上造成
       * 不精确
       */

      // First pass: detect the flex items whose min/max constraints trigger
      /*
       *  第一次循环没有做实质的 dimension 调整
       *  仅仅是预计算所有孩子伸缩 + min/max 矫正之后的增量
       *  得到矫正后的新的剩余空间和factors
       */
      float deltaFlexShrinkScaledFactors = 0;
      float deltaFlexGrowFactors = 0;
      currentRelativeChild = firstRelativeChild;
      while (currentRelativeChild != NULL) {
        childFlexBasis =
            fminf(YGResolveValue(&currentRelativeChild->style.maxDimensions[dim[mainAxis]],
                                 mainAxisParentSize),
                  fmaxf(YGResolveValue(&currentRelativeChild->style.minDimensions[dim[mainAxis]],
                                       mainAxisParentSize),
                        currentRelativeChild->layout.computedFlexBasis));

        if (remainingFreeSpace < 0) {
          flexShrinkScaledFactor = -YGNodeResolveFlexShrink(currentRelativeChild) * childFlexBasis;

          // Is this child able to shrink?
          if (flexShrinkScaledFactor != 0) {
            baseMainSize =
                childFlexBasis +
                remainingFreeSpace / totalFlexShrinkScaledFactors * flexShrinkScaledFactor;
            boundMainSize = YGNodeBoundAxis(currentRelativeChild,
                                            mainAxis,
                                            baseMainSize,
                                            availableInnerMainDim,
                                            availableInnerWidth);
            if (baseMainSize != boundMainSize) {
              // By excluding this item's size and flex factor from remaining,
              // this item's
              // min/max constraints should also trigger in the second pass
              // resulting in the
              // item's size calculation being identical in the first and second
              // passes.
              // 1. 这个 delta 是真正要分配给触发了边界纠正的节点的空间
              // 2. 循环结束后 deltaFreeSpace 是总的要分配给触发了边界纠正的节点的空间
              // 3. 循环结束后 remainingFreeSpace 是指分配给未触发边界纠正的节点的空间
              // 4. 把 factor 从总的 factors 中移除是为了让这些节点在第二次循环中一定会触发边界纠正
              // 从而确保分配给未触发边界纠正的节点的空间和第一次循环中的计算是一致的
              deltaFreeSpace -= boundMainSize - childFlexBasis;
              deltaFlexShrinkScaledFactors -= flexShrinkScaledFactor;
            }
          }
        } else if (remainingFreeSpace > 0) {
          flexGrowFactor = YGResolveFlexGrow(currentRelativeChild);

          // Is this child able to grow?
          if (flexGrowFactor != 0) {
            baseMainSize =
                childFlexBasis + remainingFreeSpace / totalFlexGrowFactors * flexGrowFactor;
            boundMainSize = YGNodeBoundAxis(currentRelativeChild,
                                            mainAxis,
                                            baseMainSize,
                                            availableInnerMainDim,
                                            availableInnerWidth);

            if (baseMainSize != boundMainSize) {
              // By excluding this item's size and flex factor from remaining,
              // this item's
              // min/max constraints should also trigger in the second pass
              // resulting in the
              // item's size calculation being identical in the first and second
              // passes.
              deltaFreeSpace -= boundMainSize - childFlexBasis;
              deltaFlexGrowFactors -= flexGrowFactor;
            }
          }
        }

        currentRelativeChild = currentRelativeChild->nextChild;
      }

      // 第一次循环结束的产物：矫正后的剩余空间（最终可分配给其他未越界孩子的空间）、grow factors、shrink factors
      totalFlexShrinkScaledFactors += deltaFlexShrinkScaledFactors;
      totalFlexGrowFactors += deltaFlexGrowFactors;
      remainingFreeSpace += deltaFreeSpace;

      // Second pass: resolve the sizes of the flexible items
      deltaFreeSpace = 0;
      currentRelativeChild = firstRelativeChild;
      while (currentRelativeChild != NULL) {
        childFlexBasis =
            fminf(YGResolveValue(&currentRelativeChild->style.maxDimensions[dim[mainAxis]],
                                 mainAxisParentSize),
                  fmaxf(YGResolveValue(&currentRelativeChild->style.minDimensions[dim[mainAxis]],
                                       mainAxisParentSize),
                        currentRelativeChild->layout.computedFlexBasis));
        float updatedMainSize = childFlexBasis;

        if (remainingFreeSpace < 0) {
          flexShrinkScaledFactor = -YGNodeResolveFlexShrink(currentRelativeChild) * childFlexBasis;
          // Is this child able to shrink?
          if (flexShrinkScaledFactor != 0) {
            float childSize;

            // 相当于在第一次循环中所有节点都越(下)界，这里强行让它继续越界
            if (totalFlexShrinkScaledFactors == 0) {
              childSize = childFlexBasis + flexShrinkScaledFactor;// 按 shrink factor < - 1 算，childSize <= 0;
            } else {
              // 第一次循环中越界和非越界的节点按相同的方式计算，但此时 totalFlexShrinkScaledFactors 中
              // 已经没有越界节点的因子，因此该计算结果的绝对值会偏大，按 yoga 注释的解释是为了让第一次循环中
              // 越界的节点在本次循环中强行保持一致（类似与在第一次循环中进行了标记）
              childSize =
                  childFlexBasis +
                  (remainingFreeSpace / totalFlexShrinkScaledFactors) * flexShrinkScaledFactor;
            }

            updatedMainSize = YGNodeBoundAxis(currentRelativeChild,
                                              mainAxis,
                                              childSize,
                                              availableInnerMainDim,
                                              availableInnerWidth);
          }
        } else if (remainingFreeSpace > 0) {
          flexGrowFactor = YGResolveFlexGrow(currentRelativeChild);

          // Is this child able to grow?
          if (flexGrowFactor != 0) {
            updatedMainSize =
                YGNodeBoundAxis(currentRelativeChild,
                                mainAxis,
                                childFlexBasis +
                                    remainingFreeSpace / totalFlexGrowFactors * flexGrowFactor,
                                availableInnerMainDim,
                                availableInnerWidth);
          }
        }

        // 二次循环后真实分配出去的空间
        deltaFreeSpace -= updatedMainSize - childFlexBasis;

        const float marginMain =
            YGNodeMarginForAxis(currentRelativeChild, mainAxis, availableInnerWidth);
        const float marginCross =
            YGNodeMarginForAxis(currentRelativeChild, crossAxis, availableInnerWidth);

        float childCrossSize;
        float childMainSize = updatedMainSize + marginMain;
        YGMeasureMode childCrossMeasureMode;
        YGMeasureMode childMainMeasureMode = YGMeasureModeExactly;

        // 按 aspectRatio 计算交叉轴 size
        if (!YGFloatIsUndefined(currentRelativeChild->style.aspectRatio)) {
          childCrossSize =
              isMainAxisRow
                  ? (childMainSize - marginMain) / currentRelativeChild->style.aspectRatio
                  : (childMainSize - marginMain) * currentRelativeChild->style.aspectRatio;
          childCrossMeasureMode = YGMeasureModeExactly;

          childCrossSize += marginCross;
        // // 按 stretch 计算交叉轴 size (这里考虑只有一行的情况)
        } else if (!YGFloatIsUndefined(availableInnerCrossDim) &&
                   !YGNodeIsStyleDimDefined(currentRelativeChild,
                                            crossAxis,
                                            availableInnerCrossDim) &&
                   measureModeCrossDim == YGMeasureModeExactly &&
                   !(isNodeFlexWrap && flexBasisOverflows) &&
                   YGNodeAlignItem(node, currentRelativeChild) == YGAlignStretch) {
          childCrossSize = availableInnerCrossDim;
          childCrossMeasureMode = YGMeasureModeExactly;
        // 按内容计算交叉轴 size
        } else if (!YGNodeIsStyleDimDefined(currentRelativeChild,
                                            crossAxis,
                                            availableInnerCrossDim)) {
          childCrossSize = availableInnerCrossDim;
          childCrossMeasureMode =
              YGFloatIsUndefined(childCrossSize) ? YGMeasureModeUndefined : YGMeasureModeAtMost;
        // 按 style 中交叉轴方向的设置计算 size
        } else {
          childCrossSize = YGResolveValue(currentRelativeChild->resolvedDimensions[dim[crossAxis]],
                                          availableInnerCrossDim) +
                           marginCross;
          const bool isLoosePercentageMeasurement =
              currentRelativeChild->resolvedDimensions[dim[crossAxis]]->unit == YGUnitPercent &&
              measureModeCrossDim != YGMeasureModeExactly;
          childCrossMeasureMode = YGFloatIsUndefined(childCrossSize) || isLoosePercentageMeasurement
                                      ? YGMeasureModeUndefined
                                      : YGMeasureModeExactly;
        }

        YGConstrainMaxSizeForMode(currentRelativeChild,
                                  mainAxis,
                                  availableInnerMainDim,
                                  availableInnerWidth,
                                  &childMainMeasureMode,
                                  &childMainSize);
        YGConstrainMaxSizeForMode(currentRelativeChild,
                                  crossAxis,
                                  availableInnerCrossDim,
                                  availableInnerWidth,
                                  &childCrossMeasureMode,
                                  &childCrossSize);

        // 对齐方式是否是 stretch
        const bool requiresStretchLayout =
            !YGNodeIsStyleDimDefined(currentRelativeChild, crossAxis, availableInnerCrossDim) &&
            YGNodeAlignItem(node, currentRelativeChild) == YGAlignStretch;

        const float childWidth = isMainAxisRow ? childMainSize : childCrossSize;
        const float childHeight = !isMainAxisRow ? childMainSize : childCrossSize;

        const YGMeasureMode childWidthMeasureMode =
            isMainAxisRow ? childMainMeasureMode : childCrossMeasureMode;
        const YGMeasureMode childHeightMeasureMode =
            !isMainAxisRow ? childMainMeasureMode : childCrossMeasureMode;

        // Recursively call the layout algorithm for this child with the updated
        // main size.
        // 到这一步 main size 是肯定已经确定下来的，但是 cross size 仍有可能是不确定的，因此还需要做内部布局
        // main size 确定之后通过递归 layout 计算 cross size
        // 如果交叉轴是 stretch 对齐方式，则 cross size 不需要做递归布局就能确定了
        YGLayoutNodeInternal(currentRelativeChild,
                             childWidth,
                             childHeight,
                             direction,
                             childWidthMeasureMode,
                             childHeightMeasureMode,
                             availableInnerWidth,
                             availableInnerHeight,
                             performLayout && !requiresStretchLayout,
                             "flex",
                             config);
        node->layout.hadOverflow |= currentRelativeChild->layout.hadOverflow;

        currentRelativeChild = currentRelativeChild->nextChild;
      }
    }

    // 至此，当前行所有 child 的 main size 和 cross size 都计算完成
    // main size 都计算完成后的剩余空间
    remainingFreeSpace = originalRemainingFreeSpace + deltaFreeSpace;
    node->layout.hadOverflow |= (remainingFreeSpace < 0);

    // STEP 6: MAIN-AXIS JUSTIFICATION & CROSS-AXIS SIZE DETERMINATION

    // At this point, all the children have their dimensions set in the main
    // axis.
    // Their dimensions are also set in the cross axis with the exception of
    // items
    // that are aligned "stretch". We need to compute these stretch values and
    // set the final positions.

    // If we are using "at most" rules in the main axis. Calculate the remaining space when
    // constraint by the min size defined for the main axis.

    // 条件分析：
    // 1 measureModeMainDim == YGMeasureModeAtMost: fit-content，node 的宽度等于内容宽度，同时不超过可用空间
    // 2 remainingFreeSpace > 0: 说明经过上面的空间分配，内容子节点的宽度已经 grow 到最大值
    if (measureModeMainDim == YGMeasureModeAtMost && remainingFreeSpace > 0) {
      // "at most" 是 fit-content，即 node 宽度等于内容宽度，同时考虑可用空间。
      // 这里会考虑这种形式下，内容宽度如果小于最小宽度的设置时，node 宽度应该等于最小宽度
      // 此时的 remaining space 就是最小宽度-内容宽度
      // availableInnerMainDim - remainingFreeSpace 即 node 内容的宽度，
      // 当内容的宽度小于 node 最小宽度时， node 最终宽度相当于会被拉长，因此其内部会多出一部分可分配空间
      // 但因为子孩子节点都已经 grow 到最大值了，所以这些空间仅在对齐时使用
      if (node->style.minDimensions[dim[mainAxis]].unit != YGUnitUndefined &&
          YGResolveValue(&node->style.minDimensions[dim[mainAxis]], mainAxisParentSize) >= 0) {
        remainingFreeSpace =
            fmaxf(0,
                  YGResolveValue(&node->style.minDimensions[dim[mainAxis]], mainAxisParentSize) -
                      (availableInnerMainDim - remainingFreeSpace));
      // 没有设置最小值时，node 节点的宽度即等于所有内容的宽度之和，因此无额外可分配空间
      } else {
        remainingFreeSpace = 0;
      }
    }

    int numberOfAutoMarginsOnCurrentLine = 0;
    for (uint32_t i = startOfLineIndex; i < endOfLineIndex; i++) {
      const YGNodeRef child = YGNodeListGet(node->children, i);
      if (child->style.positionType == YGPositionTypeRelative) {
        if (YGMarginLeadingValue(child, mainAxis)->unit == YGUnitAuto) {
          numberOfAutoMarginsOnCurrentLine++;
        }
        if (YGMarginTrailingValue(child, mainAxis)->unit == YGUnitAuto) {
          numberOfAutoMarginsOnCurrentLine++;
        }
      }
    }

    if (numberOfAutoMarginsOnCurrentLine == 0) {
      switch (justifyContent) {
        case YGJustifyCenter:
          leadingMainDim = remainingFreeSpace / 2;
          break;
        case YGJustifyFlexEnd:
          leadingMainDim = remainingFreeSpace;
          break;
        case YGJustifySpaceBetween:
          if (itemsOnLine > 1) {
            betweenMainDim = fmaxf(remainingFreeSpace, 0) / (itemsOnLine - 1);
          } else {
            betweenMainDim = 0;
          }
          break;
        case YGJustifySpaceAround:
          // Space on the edges is half of the space between elements
          betweenMainDim = remainingFreeSpace / itemsOnLine;
          leadingMainDim = betweenMainDim / 2;
          break;
        case YGJustifyFlexStart:
          break;
      }
    }

    float mainDim = leadingPaddingAndBorderMain + leadingMainDim;
    float crossDim = 0;

    // 计算每个孩子节点主轴方向的 postion
    for (uint32_t i = startOfLineIndex; i < endOfLineIndex; i++) {
      const YGNodeRef child = YGNodeListGet(node->children, i);
      if (child->style.display == YGDisplayNone) {
        continue;
      }
      if (child->style.positionType == YGPositionTypeAbsolute &&
          YGNodeIsLeadingPosDefined(child, mainAxis)) {
        if (performLayout) {
          // In case the child is position absolute and has left/top being
          // defined, we override the position to whatever the user said
          // (and margin/border).
          child->layout.position[pos[mainAxis]] =
              YGNodeLeadingPosition(child, mainAxis, availableInnerMainDim) +
              YGNodeLeadingBorder(node, mainAxis) +
              YGNodeLeadingMargin(child, mainAxis, availableInnerWidth);
        }
      } else {
        // Now that we placed the element, we need to update the variables.
        // We need to do that only for relative elements. Absolute elements
        // do not take part in that phase.
        if (child->style.positionType == YGPositionTypeRelative) {
          if (YGMarginLeadingValue(child, mainAxis)->unit == YGUnitAuto) {
            mainDim += remainingFreeSpace / numberOfAutoMarginsOnCurrentLine;
          }

          if (performLayout) {
            child->layout.position[pos[mainAxis]] += mainDim;
          }

          if (YGMarginTrailingValue(child, mainAxis)->unit == YGUnitAuto) {
            mainDim += remainingFreeSpace / numberOfAutoMarginsOnCurrentLine;
          }

          // canSkipFlex = !performLayout && measureModeCrossDim == YGMeasureModeExactly;
          if (canSkipFlex) {
            // If we skipped the flex step, then we can't rely on the
            // measuredDims because
            // they weren't computed. This means we can't call YGNodeDimWithMargin.
            mainDim += betweenMainDim + YGNodeMarginForAxis(child, mainAxis, availableInnerWidth) +
                       child->layout.computedFlexBasis;
            crossDim = availableInnerCrossDim;
          } else {
            // The main dimension is the sum of all the elements dimension plus the spacing.
            mainDim += betweenMainDim + YGNodeDimWithMargin(child, mainAxis, availableInnerWidth);

            // The cross dimension is the max of the elements dimension since
            // there can only be one element in that cross dimension.
            crossDim = fmaxf(crossDim, YGNodeDimWithMargin(child, crossAxis, availableInnerWidth));
          }
        } else if (performLayout) {
          child->layout.position[pos[mainAxis]] +=
              YGNodeLeadingBorder(node, mainAxis) + leadingMainDim;
        }
      }
    }

    mainDim += trailingPaddingAndBorderMain;

    float containerCrossAxis = availableInnerCrossDim;
    if (measureModeCrossDim == YGMeasureModeUndefined ||
        measureModeCrossDim == YGMeasureModeAtMost) {
      // Compute the cross axis from the max cross dimension of the children.
      containerCrossAxis = YGNodeBoundAxis(node,
                                           crossAxis,
                                           crossDim + paddingAndBorderAxisCross,
                                           crossAxisParentSize,
                                           parentWidth) -
                           paddingAndBorderAxisCross;
    }

    // If there's no flex wrap, the cross dimension is defined by the container.
    if (!isNodeFlexWrap && measureModeCrossDim == YGMeasureModeExactly) {
      crossDim = availableInnerCrossDim;
    }

    // Clamp to the min/max size specified on the container.
    crossDim = YGNodeBoundAxis(node,
                               crossAxis,
                               crossDim + paddingAndBorderAxisCross,
                               crossAxisParentSize,
                               parentWidth) -
               paddingAndBorderAxisCross;

    // STEP 7: CROSS-AXIS ALIGNMENT
    // We can skip child alignment if we're just measuring the container.
    if (performLayout) {
      for (uint32_t i = startOfLineIndex; i < endOfLineIndex; i++) {
        const YGNodeRef child = YGNodeListGet(node->children, i);
        if (child->style.display == YGDisplayNone) {
          continue;
        }
        if (child->style.positionType == YGPositionTypeAbsolute) {
          // If the child is absolutely positioned and has a
          // top/left/bottom/right
          // set, override all the previously computed positions to set it
          // correctly.
          if (YGNodeIsLeadingPosDefined(child, crossAxis)) {
            child->layout.position[pos[crossAxis]] =
                YGNodeLeadingPosition(child, crossAxis, availableInnerCrossDim) +
                YGNodeLeadingBorder(node, crossAxis) +
                YGNodeLeadingMargin(child, crossAxis, availableInnerWidth);
          } else {
            child->layout.position[pos[crossAxis]] =
                YGNodeLeadingBorder(node, crossAxis) +
                YGNodeLeadingMargin(child, crossAxis, availableInnerWidth);
          }
        } else {
          float leadingCrossDim = leadingPaddingAndBorderCross;

          // For a relative children, we're either using alignItems (parent) or
          // alignSelf (child) in order to determine the position in the cross
          // axis
          const YGAlign alignItem = YGNodeAlignItem(node, child);

          // If the child uses align stretch, we need to lay it out one more
          // time, this time
          // forcing the cross-axis size to be the computed cross size for the
          // current line.
          if (alignItem == YGAlignStretch &&
              YGMarginLeadingValue(child, crossAxis)->unit != YGUnitAuto &&
              YGMarginTrailingValue(child, crossAxis)->unit != YGUnitAuto) {
            // If the child defines a definite size for its cross axis, there's
            // no need to stretch.
            if (!YGNodeIsStyleDimDefined(child, crossAxis, availableInnerCrossDim)) {
              float childMainSize = child->layout.measuredDimensions[dim[mainAxis]];
              float childCrossSize =
                  !YGFloatIsUndefined(child->style.aspectRatio)
                      ? ((YGNodeMarginForAxis(child, crossAxis, availableInnerWidth) +
                          (isMainAxisRow ? childMainSize / child->style.aspectRatio
                                         : childMainSize * child->style.aspectRatio)))
                      : crossDim;

              childMainSize += YGNodeMarginForAxis(child, mainAxis, availableInnerWidth);

              YGMeasureMode childMainMeasureMode = YGMeasureModeExactly;
              YGMeasureMode childCrossMeasureMode = YGMeasureModeExactly;
              YGConstrainMaxSizeForMode(child,
                                        mainAxis,
                                        availableInnerMainDim,
                                        availableInnerWidth,
                                        &childMainMeasureMode,
                                        &childMainSize);
              YGConstrainMaxSizeForMode(child,
                                        crossAxis,
                                        availableInnerCrossDim,
                                        availableInnerWidth,
                                        &childCrossMeasureMode,
                                        &childCrossSize);

              const float childWidth = isMainAxisRow ? childMainSize : childCrossSize;
              const float childHeight = !isMainAxisRow ? childMainSize : childCrossSize;

              const YGMeasureMode childWidthMeasureMode =
                  YGFloatIsUndefined(childWidth) ? YGMeasureModeUndefined : YGMeasureModeExactly;
              const YGMeasureMode childHeightMeasureMode =
                  YGFloatIsUndefined(childHeight) ? YGMeasureModeUndefined : YGMeasureModeExactly;

              YGLayoutNodeInternal(child,
                                   childWidth,
                                   childHeight,
                                   direction,
                                   childWidthMeasureMode,
                                   childHeightMeasureMode,
                                   availableInnerWidth,
                                   availableInnerHeight,
                                   true,
                                   "stretch",
                                   config);
            }
          } else {
            const float remainingCrossDim =
                containerCrossAxis - YGNodeDimWithMargin(child, crossAxis, availableInnerWidth);

            if (YGMarginLeadingValue(child, crossAxis)->unit == YGUnitAuto &&
                YGMarginTrailingValue(child, crossAxis)->unit == YGUnitAuto) {
              leadingCrossDim += fmaxf(0.0f, remainingCrossDim / 2);
            } else if (YGMarginTrailingValue(child, crossAxis)->unit == YGUnitAuto) {
              // No-Op
            } else if (YGMarginLeadingValue(child, crossAxis)->unit == YGUnitAuto) {
              leadingCrossDim += fmaxf(0.0f, remainingCrossDim);
            } else if (alignItem == YGAlignFlexStart) {
              // No-Op
            } else if (alignItem == YGAlignCenter) {
              leadingCrossDim += remainingCrossDim / 2;
            } else {
              leadingCrossDim += remainingCrossDim;
            }
          }
          // And we apply the position
          child->layout.position[pos[crossAxis]] += totalLineCrossDim + leadingCrossDim;
        }
      }
    }

    // 交叉轴总 size
    totalLineCrossDim += crossDim;
    maxLineMainDim = fmaxf(maxLineMainDim, mainDim);
  }

  // STEP 8: MULTI-LINE CONTENT ALIGNMENT
  if (performLayout && (lineCount > 1 || YGIsBaselineLayout(node)) &&
      !YGFloatIsUndefined(availableInnerCrossDim)) {
    const float remainingAlignContentDim = availableInnerCrossDim - totalLineCrossDim;

    float crossDimLead = 0;
    float currentLead = leadingPaddingAndBorderCross;

    switch (node->style.alignContent) {
      case YGAlignFlexEnd:
        currentLead += remainingAlignContentDim;
        break;
      case YGAlignCenter:
        currentLead += remainingAlignContentDim / 2;
        break;
      case YGAlignStretch:
        if (availableInnerCrossDim > totalLineCrossDim) {
          crossDimLead = remainingAlignContentDim / lineCount;
        }
        break;
      case YGAlignSpaceAround:
        if (availableInnerCrossDim > totalLineCrossDim) {
          currentLead += remainingAlignContentDim / (2 * lineCount);
          if (lineCount > 1) {
            crossDimLead = remainingAlignContentDim / lineCount;
          }
        } else {
          currentLead += remainingAlignContentDim / 2;
        }
        break;
      case YGAlignSpaceBetween:
        if (availableInnerCrossDim > totalLineCrossDim && lineCount > 1) {
          crossDimLead = remainingAlignContentDim / (lineCount - 1);
        }
        break;
      case YGAlignAuto:
      case YGAlignFlexStart:
      case YGAlignBaseline:
        break;
    }

    uint32_t endIndex = 0;
    for (uint32_t i = 0; i < lineCount; i++) {
      const uint32_t startIndex = endIndex;
      uint32_t ii;

      // compute the line's height and find the endIndex
      float lineHeight = 0;
      float maxAscentForCurrentLine = 0;
      float maxDescentForCurrentLine = 0;
      for (ii = startIndex; ii < childCount; ii++) {
        const YGNodeRef child = YGNodeListGet(node->children, ii);
        if (child->style.display == YGDisplayNone) {
          continue;
        }
        if (child->style.positionType == YGPositionTypeRelative) {
          if (child->lineIndex != i) {
            break;
          }
          if (YGNodeIsLayoutDimDefined(child, crossAxis)) {
            lineHeight = fmaxf(lineHeight,
                               child->layout.measuredDimensions[dim[crossAxis]] +
                                   YGNodeMarginForAxis(child, crossAxis, availableInnerWidth));
          }
          if (YGNodeAlignItem(node, child) == YGAlignBaseline) {
            const float ascent =
                YGBaseline(child) +
                YGNodeLeadingMargin(child, YGFlexDirectionColumn, availableInnerWidth);
            const float descent =
                child->layout.measuredDimensions[YGDimensionHeight] +
                YGNodeMarginForAxis(child, YGFlexDirectionColumn, availableInnerWidth) - ascent;
            maxAscentForCurrentLine = fmaxf(maxAscentForCurrentLine, ascent);
            maxDescentForCurrentLine = fmaxf(maxDescentForCurrentLine, descent);
            lineHeight = fmaxf(lineHeight, maxAscentForCurrentLine + maxDescentForCurrentLine);
          }
        }
      }
      endIndex = ii;
      lineHeight += crossDimLead;

      if (performLayout) {
        for (ii = startIndex; ii < endIndex; ii++) {
          const YGNodeRef child = YGNodeListGet(node->children, ii);
          if (child->style.display == YGDisplayNone) {
            continue;
          }
          if (child->style.positionType == YGPositionTypeRelative) {
            switch (YGNodeAlignItem(node, child)) {
              case YGAlignFlexStart: {
                child->layout.position[pos[crossAxis]] =
                    currentLead + YGNodeLeadingMargin(child, crossAxis, availableInnerWidth);
                break;
              }
              case YGAlignFlexEnd: {
                child->layout.position[pos[crossAxis]] =
                    currentLead + lineHeight -
                    YGNodeTrailingMargin(child, crossAxis, availableInnerWidth) -
                    child->layout.measuredDimensions[dim[crossAxis]];
                break;
              }
              case YGAlignCenter: {
                float childHeight = child->layout.measuredDimensions[dim[crossAxis]];
                child->layout.position[pos[crossAxis]] =
                    currentLead + (lineHeight - childHeight) / 2;
                break;
              }
              case YGAlignStretch: {
                child->layout.position[pos[crossAxis]] =
                    currentLead + YGNodeLeadingMargin(child, crossAxis, availableInnerWidth);

                // Remeasure child with the line height as it as been only measured with the
                // parents height yet.
                if (!YGNodeIsStyleDimDefined(child, crossAxis, availableInnerCrossDim)) {
                  const float childWidth =
                      isMainAxisRow ? (child->layout.measuredDimensions[YGDimensionWidth] +
                                       YGNodeMarginForAxis(child, mainAxis, availableInnerWidth))
                                    : lineHeight;

                  const float childHeight =
                      !isMainAxisRow ? (child->layout.measuredDimensions[YGDimensionHeight] +
                                        YGNodeMarginForAxis(child, crossAxis, availableInnerWidth))
                                     : lineHeight;

                  if (!(YGFloatsEqual(childWidth,
                                      child->layout.measuredDimensions[YGDimensionWidth]) &&
                        YGFloatsEqual(childHeight,
                                      child->layout.measuredDimensions[YGDimensionHeight]))) {
                    YGLayoutNodeInternal(child,
                                         childWidth,
                                         childHeight,
                                         direction,
                                         YGMeasureModeExactly,
                                         YGMeasureModeExactly,
                                         availableInnerWidth,
                                         availableInnerHeight,
                                         true,
                                         "multiline-stretch",
                                         config);
                  }
                }
                break;
              }
              case YGAlignBaseline: {
                child->layout.position[YGEdgeTop] =
                    currentLead + maxAscentForCurrentLine - YGBaseline(child) +
                    YGNodeLeadingPosition(child, YGFlexDirectionColumn, availableInnerCrossDim);
                break;
              }
              case YGAlignAuto:
              case YGAlignSpaceBetween:
              case YGAlignSpaceAround:
                break;
            }
          }
        }
      }

      currentLead += lineHeight;
    }
  }

  // STEP 9: COMPUTING FINAL DIMENSIONS
  node->layout.measuredDimensions[YGDimensionWidth] = YGNodeBoundAxis(
      node, YGFlexDirectionRow, availableWidth - marginAxisRow, parentWidth, parentWidth);
  node->layout.measuredDimensions[YGDimensionHeight] = YGNodeBoundAxis(
      node, YGFlexDirectionColumn, availableHeight - marginAxisColumn, parentHeight, parentWidth);

  // If the user didn't specify a width or height for the node, set the
  // dimensions based on the children.
  /*
   * 这里两个条件
   * 1. 没有提供明确的可用空间（比如没有指定宽度），此时只需要考虑内容的 size
   * 2. 提供了明确的可用空间（比如指定了宽度），但是需要按 fit-content 计算，此时还需要考虑 overflow 情况
   *  a. 如果 overflow 非 scroll，则同第1中情况，即内容即使超过了可用空间也按内容计算，这和 css 中是一致的
   *  b. 如果 overflow 为 scroll，则 node 最大 size 不超过可用空间，因为超过了会出滚动条
   */
  if (measureModeMainDim == YGMeasureModeUndefined ||
      (node->style.overflow != YGOverflowScroll && measureModeMainDim == YGMeasureModeAtMost)) {
    // Clamp the size to the min/max size, if specified, and make sure it
    // doesn't go below the padding and border amount.
    node->layout.measuredDimensions[dim[mainAxis]] =
        YGNodeBoundAxis(node, mainAxis, maxLineMainDim, mainAxisParentSize, parentWidth);
  // 如果指定了可用空间，并且 overflow 为 scroll，则 main dimension 最大不超过可用空间（可以出滚动条）
  // 最小不小于其 padding + border
  } else if (measureModeMainDim == YGMeasureModeAtMost &&
             node->style.overflow == YGOverflowScroll) {
    node->layout.measuredDimensions[dim[mainAxis]] = fmaxf(
        fminf(availableInnerMainDim + paddingAndBorderAxisMain,
              YGNodeBoundAxisWithinMinAndMax(node, mainAxis, maxLineMainDim, mainAxisParentSize)),
        paddingAndBorderAxisMain);
  }

  if (measureModeCrossDim == YGMeasureModeUndefined ||
      (node->style.overflow != YGOverflowScroll && measureModeCrossDim == YGMeasureModeAtMost)) {
    // Clamp the size to the min/max size, if specified, and make sure it
    // doesn't go below the padding and border amount.
    node->layout.measuredDimensions[dim[crossAxis]] =
        YGNodeBoundAxis(node,
                        crossAxis,
                        totalLineCrossDim + paddingAndBorderAxisCross,
                        crossAxisParentSize,
                        parentWidth);
  // 如果指定了可用空间，并且 overflow 为 scroll，则 cross dimension 最大不超过可用空间（可以出滚动条）
  // 最小不小于其 padding + border
  } else if (measureModeCrossDim == YGMeasureModeAtMost &&
             node->style.overflow == YGOverflowScroll) {
    node->layout.measuredDimensions[dim[crossAxis]] =
        fmaxf(fminf(availableInnerCrossDim + paddingAndBorderAxisCross,
                    YGNodeBoundAxisWithinMinAndMax(node,
                                                   crossAxis,
                                                   totalLineCrossDim + paddingAndBorderAxisCross,
                                                   crossAxisParentSize)),
              paddingAndBorderAxisCross);
  }

  // As we only wrapped in normal direction yet, we need to reverse the positions on wrap-reverse.
  if (performLayout && node->style.flexWrap == YGWrapWrapReverse) {
    for (uint32_t i = 0; i < childCount; i++) {
      const YGNodeRef child = YGNodeGetChild(node, i);
      if (child->style.positionType == YGPositionTypeRelative) {
        child->layout.position[pos[crossAxis]] = node->layout.measuredDimensions[dim[crossAxis]] -
                                                 child->layout.position[pos[crossAxis]] -
                                                 child->layout.measuredDimensions[dim[crossAxis]];
      }
    }
  }

  if (performLayout) {
    // STEP 10: SIZING AND POSITIONING ABSOLUTE CHILDREN
    for (currentAbsoluteChild = firstAbsoluteChild; currentAbsoluteChild != NULL;
         currentAbsoluteChild = currentAbsoluteChild->nextChild) {
      YGNodeAbsoluteLayoutChild(node,
                                currentAbsoluteChild,
                                availableInnerWidth,
                                isMainAxisRow ? measureModeMainDim : measureModeCrossDim,
                                availableInnerHeight,
                                direction,
                                config);
    }

    // STEP 11: SETTING TRAILING POSITIONS FOR CHILDREN
    const bool needsMainTrailingPos =
        mainAxis == YGFlexDirectionRowReverse || mainAxis == YGFlexDirectionColumnReverse;
    const bool needsCrossTrailingPos =
        crossAxis == YGFlexDirectionRowReverse || crossAxis == YGFlexDirectionColumnReverse;

    // Set trailing position if necessary.
    if (needsMainTrailingPos || needsCrossTrailingPos) {
      for (uint32_t i = 0; i < childCount; i++) {
        const YGNodeRef child = YGNodeListGet(node->children, i);
        if (child->style.display == YGDisplayNone) {
          continue;
        }
        if (needsMainTrailingPos) {
          YGNodeSetChildTrailingPosition(node, child, mainAxis);
        }

        if (needsCrossTrailingPos) {
          YGNodeSetChildTrailingPosition(node, child, crossAxis);
        }
      }
    }
  }
}

uint32_t gDepth = 0;
bool gPrintTree = false;
bool gPrintChanges = false;
bool gPrintSkips = false;

static const char *spacer = "                                                            ";

static const char *YGSpacer(const unsigned long level) {
  const size_t spacerLen = strlen(spacer);
  if (level > spacerLen) {
    return &spacer[0];
  } else {
    return &spacer[spacerLen - level];
  }
}

static const char *YGMeasureModeName(const YGMeasureMode mode, const bool performLayout) {
  const char *kMeasureModeNames[YGMeasureModeCount] = {"UNDEFINED", "EXACTLY", "AT_MOST"};
  const char *kLayoutModeNames[YGMeasureModeCount] = {"LAY_UNDEFINED",
                                                      "LAY_EXACTLY",
                                                      "LAY_AT_"
                                                      "MOST"};

  if (mode >= YGMeasureModeCount) {
    return "";
  }

  return performLayout ? kLayoutModeNames[mode] : kMeasureModeNames[mode];
}

static inline bool YGMeasureModeSizeIsExactAndMatchesOldMeasuredSize(YGMeasureMode sizeMode,
                                                                     float size,
                                                                     float lastComputedSize) {
  return sizeMode == YGMeasureModeExactly && YGFloatsEqual(size, lastComputedSize);
}

/*
 * Measure Mode 从 YGMeasureModeUndefined(完全按照内容 size 计算，无约束)
 * 变成 YGMeasureModeAtMost（按照内容和可用空间计算，有可用空间约束）
 * 但可用空间大于内容 size，因此不需要重新 layout 子节点（ node 的最终 size 和 cache 一致）
 */
static inline bool YGMeasureModeOldSizeIsUnspecifiedAndStillFits(YGMeasureMode sizeMode,
                                                                 float size,
                                                                 YGMeasureMode lastSizeMode,
                                                                 float lastComputedSize) {
  return sizeMode == YGMeasureModeAtMost && lastSizeMode == YGMeasureModeUndefined &&
         (size >= lastComputedSize || YGFloatsEqual(size, lastComputedSize));
}

/*
 * Measure Mode 未变化，都是 YGMeasureModeAtMost (按照内容 size 计算，以及做可用空间约束)
 * 可用空间变小了，但是可用空间仍然大于之前计算的内容 size，因此新一轮的 size 仍然是内容的 size，所以可以命中 cache
 */
static inline bool YGMeasureModeNewMeasureSizeIsStricterAndStillValid(YGMeasureMode sizeMode,
                                                                      float size,
                                                                      YGMeasureMode lastSizeMode,
                                                                      float lastSize,
                                                                      float lastComputedSize) {
  return lastSizeMode == YGMeasureModeAtMost && sizeMode == YGMeasureModeAtMost &&
         lastSize > size && (lastComputedSize <= size || YGFloatsEqual(size, lastComputedSize));
}

float YGRoundValueToPixelGrid(const float value,
                              const float pointScaleFactor,
                              const bool forceCeil,
                              const bool forceFloor) {
  float scaledValue = value * pointScaleFactor;
  float fractial = fmodf(scaledValue, 1.0);
  if (YGFloatsEqual(fractial, 0)) {
    // First we check if the value is already rounded
    scaledValue = scaledValue - fractial;
  } else if (YGFloatsEqual(fractial, 1.0)) {
    scaledValue = scaledValue - fractial + 1.0;
  } else if (forceCeil) {
    // Next we check if we need to use forced rounding
    scaledValue = scaledValue - fractial + 1.0f;
  } else if (forceFloor) {
    scaledValue = scaledValue - fractial;
  } else {
    // Finally we just round the value
    scaledValue = scaledValue - fractial + (fractial >= 0.5f ? 1.0f : 0.0f);
  }
  return scaledValue / pointScaleFactor;
}

bool YGNodeCanUseCachedMeasurement(const YGMeasureMode widthMode,
                                   const float width,
                                   const YGMeasureMode heightMode,
                                   const float height,
                                   const YGMeasureMode lastWidthMode,
                                   const float lastWidth,
                                   const YGMeasureMode lastHeightMode,
                                   const float lastHeight,
                                   const float lastComputedWidth,
                                   const float lastComputedHeight,
                                   const float marginRow,
                                   const float marginColumn,
                                   const YGConfigRef config) {
  if (lastComputedHeight < 0 || lastComputedWidth < 0) {
    return false;
  }
  bool useRoundedComparison = config != NULL && config->pointScaleFactor != 0;
  const float effectiveWidth =
      useRoundedComparison ? YGRoundValueToPixelGrid(width, config->pointScaleFactor, false, false)
                           : width;
  const float effectiveHeight =
      useRoundedComparison ? YGRoundValueToPixelGrid(height, config->pointScaleFactor, false, false)
                           : height;
  const float effectiveLastWidth =
      useRoundedComparison
          ? YGRoundValueToPixelGrid(lastWidth, config->pointScaleFactor, false, false)
          : lastWidth;
  const float effectiveLastHeight =
      useRoundedComparison
          ? YGRoundValueToPixelGrid(lastHeight, config->pointScaleFactor, false, false)
          : lastHeight;

  const bool hasSameWidthSpec =
      lastWidthMode == widthMode && YGFloatsEqual(effectiveLastWidth, effectiveWidth);
  const bool hasSameHeightSpec =
      lastHeightMode == heightMode && YGFloatsEqual(effectiveLastHeight, effectiveHeight);

  const bool widthIsCompatible =
      hasSameWidthSpec || YGMeasureModeSizeIsExactAndMatchesOldMeasuredSize(widthMode,
                                                                            width - marginRow,
                                                                            lastComputedWidth) ||
      YGMeasureModeOldSizeIsUnspecifiedAndStillFits(widthMode,
                                                    width - marginRow,
                                                    lastWidthMode,
                                                    lastComputedWidth) ||
      YGMeasureModeNewMeasureSizeIsStricterAndStillValid(
          widthMode, width - marginRow, lastWidthMode, lastWidth, lastComputedWidth);

  const bool heightIsCompatible =
      hasSameHeightSpec || YGMeasureModeSizeIsExactAndMatchesOldMeasuredSize(heightMode,
                                                                             height - marginColumn,
                                                                             lastComputedHeight) ||
      YGMeasureModeOldSizeIsUnspecifiedAndStillFits(heightMode,
                                                    height - marginColumn,
                                                    lastHeightMode,
                                                    lastComputedHeight) ||
      YGMeasureModeNewMeasureSizeIsStricterAndStillValid(
          heightMode, height - marginColumn, lastHeightMode, lastHeight, lastComputedHeight);

  return widthIsCompatible && heightIsCompatible;
}

//
// This is a wrapper around the YGNodelayoutImpl function. It determines
// whether the layout request is redundant and can be skipped.
//
// Parameters:
//  Input parameters are the same as YGNodelayoutImpl (see above)
//  Return parameter is true if layout was performed, false if skipped
//
bool YGLayoutNodeInternal(const YGNodeRef node,
                          const float availableWidth,
                          const float availableHeight,
                          const YGDirection parentDirection,
                          const YGMeasureMode widthMeasureMode,
                          const YGMeasureMode heightMeasureMode,
                          const float parentWidth,
                          const float parentHeight,
                          const bool performLayout,
                          const char *reason,
                          const YGConfigRef config) {
  YGLayout *layout = &node->layout;

  gDepth++;

  const bool needToVisitNode =
      // isDirty 由外部接口（设置style等）触发，触发后需要调用外部 YGNodeCalculateLayout
      // 才能重新布局（gCurrentGenerationCount 只有在 YGNodeCalculateLayout 被调用时才+1）
      // needToVisitNode 的含义：
      // 1. 节点必须 isDirty (样式、属性等发生了变化并且未重新布局)
      // 2. 必须是新一轮的 Layout(即YGNodeCalculateLayout被调用)
      // 这里主要是列出了缓存一定会失效的情况
      (node->isDirty && layout->generationCount != gCurrentGenerationCount) ||
      layout->lastParentDirection != parentDirection;

  // needToVisitNode 为 true表示缓存一定失效
  // 为 false 时，后面会做各种校验
  if (needToVisitNode) {
    // Invalidate the cached results.
    layout->nextCachedMeasurementsIndex = 0;
    layout->cachedLayout.widthMeasureMode = (YGMeasureMode) -1;
    layout->cachedLayout.heightMeasureMode = (YGMeasureMode) -1;
    layout->cachedLayout.computedWidth = -1;
    layout->cachedLayout.computedHeight = -1;
  }

  YGCachedMeasurement *cachedResults = NULL;

  // Determine whether the results are already cached. We maintain a separate
  // cache for layouts and measurements. A layout operation modifies the
  // positions
  // and dimensions for nodes in the subtree. The algorithm assumes that each
  // node
  // gets layed out a maximum of one time per tree layout, but multiple
  // measurements
  // may be required to resolve all of the flex dimensions.
  // We handle nodes with measure functions specially here because they are the
  // most
  // expensive to measure, so it's worth avoiding redundant measurements if at
  // all possible.
  if (node->measure) {
    const float marginAxisRow = YGNodeMarginForAxis(node, YGFlexDirectionRow, parentWidth);
    const float marginAxisColumn = YGNodeMarginForAxis(node, YGFlexDirectionColumn, parentWidth);

    // First, try to use the layout cache.
    if (YGNodeCanUseCachedMeasurement(widthMeasureMode,
                                      availableWidth,
                                      heightMeasureMode,
                                      availableHeight,
                                      layout->cachedLayout.widthMeasureMode,
                                      layout->cachedLayout.availableWidth,
                                      layout->cachedLayout.heightMeasureMode,
                                      layout->cachedLayout.availableHeight,
                                      layout->cachedLayout.computedWidth,
                                      layout->cachedLayout.computedHeight,
                                      marginAxisRow,
                                      marginAxisColumn,
                                      config)) {
      cachedResults = &layout->cachedLayout;
    } else {
      // Try to use the measurement cache.
      for (uint32_t i = 0; i < layout->nextCachedMeasurementsIndex; i++) {
        if (YGNodeCanUseCachedMeasurement(widthMeasureMode,
                                          availableWidth,
                                          heightMeasureMode,
                                          availableHeight,
                                          layout->cachedMeasurements[i].widthMeasureMode,
                                          layout->cachedMeasurements[i].availableWidth,
                                          layout->cachedMeasurements[i].heightMeasureMode,
                                          layout->cachedMeasurements[i].availableHeight,
                                          layout->cachedMeasurements[i].computedWidth,
                                          layout->cachedMeasurements[i].computedHeight,
                                          marginAxisRow,
                                          marginAxisColumn,
                                          config)) {
          cachedResults = &layout->cachedMeasurements[i];
          break;
        }
      }
    }
  } else if (performLayout) {
    if (YGFloatsEqual(layout->cachedLayout.availableWidth, availableWidth) &&
        YGFloatsEqual(layout->cachedLayout.availableHeight, availableHeight) &&
        layout->cachedLayout.widthMeasureMode == widthMeasureMode &&
        layout->cachedLayout.heightMeasureMode == heightMeasureMode) {
      cachedResults = &layout->cachedLayout;
    }
  } else {
    for (uint32_t i = 0; i < layout->nextCachedMeasurementsIndex; i++) {
      if (YGFloatsEqual(layout->cachedMeasurements[i].availableWidth, availableWidth) &&
          YGFloatsEqual(layout->cachedMeasurements[i].availableHeight, availableHeight) &&
          layout->cachedMeasurements[i].widthMeasureMode == widthMeasureMode &&
          layout->cachedMeasurements[i].heightMeasureMode == heightMeasureMode) {
        cachedResults = &layout->cachedMeasurements[i];
        break;
      }
    }
  }

  if (!needToVisitNode && cachedResults != NULL) {
    layout->measuredDimensions[YGDimensionWidth] = cachedResults->computedWidth;
    layout->measuredDimensions[YGDimensionHeight] = cachedResults->computedHeight;

    if (gPrintChanges && gPrintSkips) {
      printf("%s%d.{[skipped] ", YGSpacer(gDepth), gDepth);
      if (node->print) {
        node->print(node);
      }
      printf("wm: %s, hm: %s, aw: %f ah: %f => d: (%f, %f) %s\n",
             YGMeasureModeName(widthMeasureMode, performLayout),
             YGMeasureModeName(heightMeasureMode, performLayout),
             availableWidth,
             availableHeight,
             cachedResults->computedWidth,
             cachedResults->computedHeight,
             reason);
    }
  } else {
    if (gPrintChanges) {
      printf("%s%d.{%s", YGSpacer(gDepth), gDepth, needToVisitNode ? "*" : "");
      if (node->print) {
        node->print(node);
      }
      printf("wm: %s, hm: %s, aw: %f ah: %f %s\n",
             YGMeasureModeName(widthMeasureMode, performLayout),
             YGMeasureModeName(heightMeasureMode, performLayout),
             availableWidth,
             availableHeight,
             reason);
    }

    YGNodelayoutImpl(node,
                     availableWidth,
                     availableHeight,
                     parentDirection,
                     widthMeasureMode,
                     heightMeasureMode,
                     parentWidth,
                     parentHeight,
                     performLayout,
                     config);

    if (gPrintChanges) {
      printf("%s%d.}%s", YGSpacer(gDepth), gDepth, needToVisitNode ? "*" : "");
      if (node->print) {
        node->print(node);
      }
      printf("wm: %s, hm: %s, d: (%f, %f) %s\n",
             YGMeasureModeName(widthMeasureMode, performLayout),
             YGMeasureModeName(heightMeasureMode, performLayout),
             layout->measuredDimensions[YGDimensionWidth],
             layout->measuredDimensions[YGDimensionHeight],
             reason);
    }

    layout->lastParentDirection = parentDirection;

    if (cachedResults == NULL) {
      if (layout->nextCachedMeasurementsIndex == YG_MAX_CACHED_RESULT_COUNT) {
        if (gPrintChanges) {
          printf("Out of cache entries!\n");
        }
        layout->nextCachedMeasurementsIndex = 0;
      }

      YGCachedMeasurement *newCacheEntry;
      if (performLayout) {
        // Use the single layout cache entry.
        newCacheEntry = &layout->cachedLayout;
      } else {
        // Allocate a new measurement cache entry.
        newCacheEntry = &layout->cachedMeasurements[layout->nextCachedMeasurementsIndex];
        layout->nextCachedMeasurementsIndex++;
      }

      newCacheEntry->availableWidth = availableWidth;
      newCacheEntry->availableHeight = availableHeight;
      newCacheEntry->widthMeasureMode = widthMeasureMode;
      newCacheEntry->heightMeasureMode = heightMeasureMode;
      newCacheEntry->computedWidth = layout->measuredDimensions[YGDimensionWidth];
      newCacheEntry->computedHeight = layout->measuredDimensions[YGDimensionHeight];
    }
  }

  if (performLayout) {
    node->layout.dimensions[YGDimensionWidth] = node->layout.measuredDimensions[YGDimensionWidth];
    node->layout.dimensions[YGDimensionHeight] = node->layout.measuredDimensions[YGDimensionHeight];
    node->hasNewLayout = true;
    node->isDirty = false;
  }

  gDepth--;

  layout->generationCount = gCurrentGenerationCount;
  return (needToVisitNode || cachedResults == NULL);
}

void YGConfigSetPointScaleFactor(const YGConfigRef config, const float pixelsInPoint) {
  YGAssertWithConfig(config, pixelsInPoint >= 0.0f, "Scale factor should not be less than zero");

  // We store points for Pixel as we will use it for rounding
  if (pixelsInPoint == 0.0f) {
    // Zero is used to skip rounding
    config->pointScaleFactor = 0.0f;
  } else {
    config->pointScaleFactor = pixelsInPoint;
  }
}

static void YGRoundToPixelGrid(const YGNodeRef node,
                               const float pointScaleFactor,
                               const float absoluteLeft,
                               const float absoluteTop) {
  if (pointScaleFactor == 0.0f) {
    return;
  }

  const float nodeLeft = node->layout.position[YGEdgeLeft];
  const float nodeTop = node->layout.position[YGEdgeTop];

  const float nodeWidth = node->layout.dimensions[YGDimensionWidth];
  const float nodeHeight = node->layout.dimensions[YGDimensionHeight];

  const float absoluteNodeLeft = absoluteLeft + nodeLeft;
  const float absoluteNodeTop = absoluteTop + nodeTop;

  const float absoluteNodeRight = absoluteNodeLeft + nodeWidth;
  const float absoluteNodeBottom = absoluteNodeTop + nodeHeight;

  // If a node has a custom measure function we never want to round down its size as this could
  // lead to unwanted text truncation.
  const bool textRounding = node->nodeType == YGNodeTypeText;

  node->layout.position[YGEdgeLeft] =
      YGRoundValueToPixelGrid(nodeLeft, pointScaleFactor, false, textRounding);
  node->layout.position[YGEdgeTop] =
      YGRoundValueToPixelGrid(nodeTop, pointScaleFactor, false, textRounding);

  // We multiply dimension by scale factor and if the result is close to the whole number, we don't
  // have any fraction
  // To verify if the result is close to whole number we want to check both floor and ceil numbers
  const bool hasFractionalWidth = !YGFloatsEqual(fmodf(nodeWidth * pointScaleFactor, 1.0), 0) &&
                                  !YGFloatsEqual(fmodf(nodeWidth * pointScaleFactor, 1.0), 1.0);
  const bool hasFractionalHeight = !YGFloatsEqual(fmodf(nodeHeight * pointScaleFactor, 1.0), 0) &&
                                   !YGFloatsEqual(fmodf(nodeHeight * pointScaleFactor, 1.0), 1.0);

  node->layout.dimensions[YGDimensionWidth] =
      YGRoundValueToPixelGrid(absoluteNodeRight,
                              pointScaleFactor,
                              (textRounding && hasFractionalWidth),
                              (textRounding && !hasFractionalWidth)) -
      YGRoundValueToPixelGrid(absoluteNodeLeft, pointScaleFactor, false, textRounding);
  node->layout.dimensions[YGDimensionHeight] =
      YGRoundValueToPixelGrid(absoluteNodeBottom,
                              pointScaleFactor,
                              (textRounding && hasFractionalHeight),
                              (textRounding && !hasFractionalHeight)) -
      YGRoundValueToPixelGrid(absoluteNodeTop, pointScaleFactor, false, textRounding);

  const uint32_t childCount = YGNodeListCount(node->children);
  for (uint32_t i = 0; i < childCount; i++) {
    YGRoundToPixelGrid(YGNodeGetChild(node, i), pointScaleFactor, absoluteNodeLeft, absoluteNodeTop);
  }
}

// parentWidth: 当 node 没有固定宽度时，parentWidth 作为其布局的 availableWidth
// 否则 availableWidth 由 node style 上设置的宽度决定
void YGNodeCalculateLayout(const YGNodeRef node,
                           const float parentWidth,
                           const float parentHeight,
                           const YGDirection parentDirection) {
  // Increment the generation count. This will force the recursive routine to
  // visit
  // all dirty nodes at least once. Subsequent visits will be skipped if the
  // input
  // parameters don't change.
  gCurrentGenerationCount++;

  YGResolveDimensions(node);

  float width = YGUndefined;
  YGMeasureMode widthMeasureMode = YGMeasureModeUndefined;
  if (YGNodeIsStyleDimDefined(node, YGFlexDirectionRow, parentWidth)) {
    width = YGResolveValue(node->resolvedDimensions[dim[YGFlexDirectionRow]], parentWidth) +
            YGNodeMarginForAxis(node, YGFlexDirectionRow, parentWidth);
    widthMeasureMode = YGMeasureModeExactly;
  } else if (YGResolveValue(&node->style.maxDimensions[YGDimensionWidth], parentWidth) >= 0.0f) {
    width = YGResolveValue(&node->style.maxDimensions[YGDimensionWidth], parentWidth);
    widthMeasureMode = YGMeasureModeAtMost;
  } else {
    width = parentWidth;
    widthMeasureMode = YGFloatIsUndefined(width) ? YGMeasureModeUndefined : YGMeasureModeExactly;
  }

  float height = YGUndefined;
  YGMeasureMode heightMeasureMode = YGMeasureModeUndefined;
  if (YGNodeIsStyleDimDefined(node, YGFlexDirectionColumn, parentHeight)) {
    height = YGResolveValue(node->resolvedDimensions[dim[YGFlexDirectionColumn]], parentHeight) +
             YGNodeMarginForAxis(node, YGFlexDirectionColumn, parentWidth);
    heightMeasureMode = YGMeasureModeExactly;
  } else if (YGResolveValue(&node->style.maxDimensions[YGDimensionHeight], parentHeight) >= 0.0f) {
    height = YGResolveValue(&node->style.maxDimensions[YGDimensionHeight], parentHeight);
    heightMeasureMode = YGMeasureModeAtMost;
  } else {
    height = parentHeight;
    heightMeasureMode = YGFloatIsUndefined(height) ? YGMeasureModeUndefined : YGMeasureModeExactly;
  }

  if (YGLayoutNodeInternal(node,
                           width,
                           height,
                           parentDirection,
                           widthMeasureMode,
                           heightMeasureMode,
                           parentWidth,
                           parentHeight,
                           true,
                           "initial",
                           node->config)) {
    YGNodeSetPosition(node, node->layout.direction, parentWidth, parentHeight, parentWidth);
    YGRoundToPixelGrid(node, node->config->pointScaleFactor, 0.0f, 0.0f);

    if (gPrintTree) {
      YGNodePrint(node, YGPrintOptionsLayout | YGPrintOptionsChildren | YGPrintOptionsStyle);
    }
  }
}

void YGConfigSetLogger(const YGConfigRef config, YGLogger logger) {
  if (logger != NULL) {
    config->logger = logger;
  } else {
#ifdef ANDROID
    config->logger = &YGAndroidLog;
#else
    config->logger = &YGDefaultLog;
#endif
  }
}

static void YGVLog(const YGConfigRef config,
                   const YGNodeRef node,
                   YGLogLevel level,
                   const char *format,
                   va_list args) {
  const YGConfigRef logConfig = config != NULL ? config : &gYGConfigDefaults;
  logConfig->logger(logConfig, node, level, format, args);

  if (level == YGLogLevelFatal) {
    abort();
  }
}

void YGLogWithConfig(const YGConfigRef config, YGLogLevel level, const char *format, ...) {
  va_list args;
  va_start(args, format);
  YGVLog(config, NULL, level, format, args);
  va_end(args);
}

void YGLog(const YGNodeRef node, YGLogLevel level, const char *format, ...) {
  va_list args;
  va_start(args, format);
  YGVLog(node == NULL ? NULL : node->config, node, level, format, args);
  va_end(args);
}

void YGAssert(const bool condition, const char *message) {
  if (!condition) {
    YGLog(NULL, YGLogLevelFatal, "%s\n", message);
  }
}

void YGAssertWithNode(const YGNodeRef node, const bool condition, const char *message) {
  if (!condition) {
    YGLog(node, YGLogLevelFatal, "%s\n", message);
  }
}

void YGAssertWithConfig(const YGConfigRef config, const bool condition, const char *message) {
  if (!condition) {
    YGLogWithConfig(config, YGLogLevelFatal, "%s\n", message);
  }
}

void YGConfigSetExperimentalFeatureEnabled(const YGConfigRef config,
                                           const YGExperimentalFeature feature,
                                           const bool enabled) {
  config->experimentalFeatures[feature] = enabled;
}

inline bool YGConfigIsExperimentalFeatureEnabled(const YGConfigRef config,
                                                 const YGExperimentalFeature feature) {
  return config->experimentalFeatures[feature];
}

void YGConfigSetUseWebDefaults(const YGConfigRef config, const bool enabled) {
  config->useWebDefaults = enabled;
}

void YGConfigSetUseLegacyStretchBehaviour(const YGConfigRef config,
                                          const bool useLegacyStretchBehaviour) {
  config->useLegacyStretchBehaviour = useLegacyStretchBehaviour;
}

bool YGConfigGetUseWebDefaults(const YGConfigRef config) {
  return config->useWebDefaults;
}

void YGConfigSetContext(const YGConfigRef config, void *context) {
  config->context = context;
}

void *YGConfigGetContext(const YGConfigRef config) {
  return config->context;
}

void YGSetMemoryFuncs(YGMalloc ygmalloc, YGCalloc yccalloc, YGRealloc ygrealloc, YGFree ygfree) {
  YGAssert(gNodeInstanceCount == 0 && gConfigInstanceCount == 0,
           "Cannot set memory functions: all node must be freed first");
  YGAssert((ygmalloc == NULL && yccalloc == NULL && ygrealloc == NULL && ygfree == NULL) ||
               (ygmalloc != NULL && yccalloc != NULL && ygrealloc != NULL && ygfree != NULL),
           "Cannot set memory functions: functions must be all NULL or Non-NULL");

  if (ygmalloc == NULL || yccalloc == NULL || ygrealloc == NULL || ygfree == NULL) {
    gYGMalloc = &malloc;
    gYGCalloc = &calloc;
    gYGRealloc = &realloc;
    gYGFree = &free;
  } else {
    gYGMalloc = ygmalloc;
    gYGCalloc = yccalloc;
    gYGRealloc = ygrealloc;
    gYGFree = ygfree;
  }
}
