package org.umbriel.link.core.domain

/** Why a Link operation failed, as the UI must distinguish it. */
sealed interface LinkFailure {
    data object WrongCode : LinkFailure
    data object Unpaired : LinkFailure
    data object Unreachable : LinkFailure
    data class Other(val message: String) : LinkFailure
}

class LinkFailureException(val failure: LinkFailure) : Exception(failure.toString())

fun Throwable.linkFailure(): LinkFailure = (this as? LinkFailureException)?.failure ?: LinkFailure.Other(message.orEmpty())
