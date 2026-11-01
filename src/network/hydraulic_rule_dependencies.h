#pragma once

#include <aowis/model/hydraulic/hydraulic_controls.h>

// Rule controls are whole MODEL entities. If one of their referenced nodes or
// links disappears, the complete rule is removed in the same logical edit.
// Keep both the editor mutation and its GUI capture watch on this predicate.
namespace hydraulic_rule_dependencies
{
inline bool referencesEntity(const HydraulicControlRule &rule, const QUuid &entity_uuid)
{
    if (entity_uuid.isNull())
        return false;
    for (const HydraulicControlRulePremise &premise : rule.premises)
    {
        if (premise.object_uuid == entity_uuid)
            return true;
    }
    for (const HydraulicControlRuleAction &action : rule.actions_then)
    {
        if (action.link_uuid == entity_uuid)
            return true;
    }
    for (const HydraulicControlRuleAction &action : rule.actions_else)
    {
        if (action.link_uuid == entity_uuid)
            return true;
    }
    return false;
}
}
