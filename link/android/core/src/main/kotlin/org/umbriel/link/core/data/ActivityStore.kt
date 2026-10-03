package org.umbriel.link.core.data

import android.util.Log
import java.io.File
import org.json.JSONArray
import org.json.JSONObject
import org.umbriel.link.core.domain.OfferedFile
import org.umbriel.link.core.domain.SavedFile
import org.umbriel.link.core.domain.TransferRecord

/**
 * The transfer history across restarts, one JSON file rewritten whole. A transfer still running when the process
 * died reads back as failed, since the core does not resume it under a new process.
 */
class ActivityStore(private val file: File) {
    fun load(): List<TransferRecord> = try {
        if (!file.exists()) emptyList() else JSONArray(file.readText()).let { array ->
            (0 until array.length()).map { record(array.getJSONObject(it)) }
        }
    } catch (error: Exception) {
        Log.w(TAG, "reading the transfer history: $error")
        emptyList()
    }

    fun save(records: List<TransferRecord>) {
        val array = JSONArray(records.map(::json))
        val temporary = File(file.parentFile, "${file.name}.tmp")
        temporary.writeText(array.toString())
        if (!temporary.renameTo(file)) Log.w(TAG, "could not replace the transfer history")
    }

    private fun json(record: TransferRecord) = JSONObject()
        .put("id", record.transferId)
        .put("desktop", record.desktopName)
        .put("incoming", record.incoming ?: JSONObject.NULL)
        .put("bytes", record.bytes)
        .put("total", record.total)
        .put("status", record.status)
        .put("time", record.time)
        .put("files", JSONArray(record.files.map { JSONObject().put("name", it.name).put("size", it.size).put("uri", it.uri) }))
        .put("saved", JSONArray(record.saved.map { JSONObject().put("name", it.name).put("uri", it.uri) }))

    private fun record(json: JSONObject): TransferRecord {
        val status = json.getString("status")
        return TransferRecord(
            transferId = json.getString("id"),
            desktopName = json.optString("desktop"),
            incoming = if (json.isNull("incoming")) null else json.getBoolean("incoming"),
            files = json.getJSONArray("files").objects().map {
                OfferedFile(it.getString("name"), it.getLong("size"), it.optString("uri").ifEmpty { null })
            },
            bytes = json.optLong("bytes"),
            total = json.optLong("total"),
            status = if (status == "offered" || status == "transferring") "failed" else status,
            saved = json.getJSONArray("saved").objects().map { SavedFile(it.getString("name"), it.getString("uri")) },
            time = json.optLong("time"),
        )
    }

    private fun JSONArray.objects(): List<JSONObject> = (0 until length()).map(::getJSONObject)

    private companion object {
        const val TAG = "ActivityStore"
    }
}
