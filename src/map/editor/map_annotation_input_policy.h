#pragma once

// Mouse-button policy for the Map Annotation tool. Keep it independent of Qt
// and bgfx so the actual placement/selection rules can be regression-tested.
namespace MapAnnotationInput
{
enum class Button { Left, Right, Other };
enum class PressAction { PassThrough, Select, Place, ContextMenu };

// A context click on an annotation and a right-drag beginning on that same
// annotation must be distinguishable. Arm the shared rectangle on press,
// dispatch the context menu only if release occurs without a drag.
constexpr bool armRectangleOnAnnotationPress(bool select_tool, bool hit_annotation, Button button)
{
    return select_tool && hit_annotation && button == Button::Right;
}

constexpr PressAction classifyPress(bool annotation_tool, bool select_tool,
                                    bool hit_annotation, Button button)
{
    if (button == Button::Left && hit_annotation && (annotation_tool || select_tool))
        return PressAction::Select;
    if (button == Button::Right && hit_annotation && (annotation_tool || select_tool))
        return PressAction::ContextMenu;
    if (button == Button::Right && annotation_tool)
        return PressAction::Place;
    return PressAction::PassThrough;
}
}
