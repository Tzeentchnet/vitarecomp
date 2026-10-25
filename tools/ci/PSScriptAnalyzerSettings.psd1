@{
    Severity = @('Error', 'Warning')
    IncludeRules = @(
        'PSAvoidGlobalAliases'
        'PSAvoidUsingCmdletAliases'
        'PSAvoidUsingEmptyCatchBlock'
        'PSAvoidUsingInvokeExpression'
        'PSAvoidUsingPlainTextForPassword'
        'PSAvoidUsingWMICmdlet'
        'PSPossibleIncorrectComparisonWithNull'
        'PSUseApprovedVerbs'
        'PSUseDeclaredVarsMoreThanAssignments'
    )
}
