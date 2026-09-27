package org.umbriel.link.core.domain

/** Why a Link operation failed, as the UI must distinguish it. */
sealed interface LinkFailure {
    data object WrongCode : LinkFailure
    data object Unpaired : LinkFailure
    data object Unreachable : LinkFailure
    /** The share breaks a rule (empty, too long, a link that is not http or https). */
    data class Rejected(val reason: String) : LinkFailure
    data class Other(val message: String) : LinkFailure
}

class LinkFailureException(val failure: LinkFailure) : Exception(failure.toString())

fun Throwable.linkFailure(): LinkFailure = (this as? LinkFailureException)?.failure ?: LinkFailure.Other(message.orEmpty())
