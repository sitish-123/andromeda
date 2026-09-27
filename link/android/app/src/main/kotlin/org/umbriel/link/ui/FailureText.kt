package org.umbriel.link.ui

import android.content.res.Resources
import org.umbriel.link.R
import org.umbriel.link.core.domain.LinkFailure

fun LinkFailure.text(resources: Resources): String = when (this) {
    LinkFailure.WrongCode -> resources.getString(R.string.failure_wrong_code)
    LinkFailure.Unpaired -> resources.getString(R.string.failure_unpaired)
    LinkFailure.Unreachable -> resources.getString(R.string.failure_unreachable)
    is LinkFailure.Other -> resources.getString(R.string.failure_other, message)
}
