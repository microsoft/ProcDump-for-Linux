// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// EventPipe helper for .NET performance counter monitoring.
//
// Implements the diagnostics IPC protocol to start an EventPipe
// session, parse the nettrace binary stream, and extract
// EventCounter values.
//
//--------------------------------------------------------------------
#include "Includes.h"
#include "EventPipeHelper.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

//--------------------------------------------------------------------
// Nettrace format constants
//--------------------------------------------------------------------
static const char NETTRACE_MAGIC[] = "Nettrace";
static const char FAST_SERIALIZATION_TAG[] = "!FastSerialization.1";

// Block types (as null-terminated strings in the stream)
#define TRACE_OBJECT_TAG        "Trace"
#define METADATA_BLOCK_TAG      "MetadataBlock"
#define STACK_BLOCK_TAG         "StackBlock"
#define EVENT_BLOCK_TAG         "EventBlock"
#define SPB_BLOCK_TAG           "SPBlock"

// System.Diagnostics.Metrics provider name
#define METRICS_PROVIDER_NAME   "System.Diagnostics.Metrics"
// TimeSeriesValues keyword for System.Diagnostics.Metrics
#define METRICS_KEYWORD_TIMESERIES  0x2

// Object tags
#define FAST_SER_TAG_NULL_REF           1
#define FAST_SER_TAG_BEGIN_OBJECT       5
#define FAST_SER_TAG_END_OBJECT         6

//--------------------------------------------------------------------
// Nettrace metadata tracking
//--------------------------------------------------------------------
#define MAX_METADATA_ENTRIES 256
static const size_t MAX_NETTRACE_BLOCK_SIZE = 64 * 1024 * 1024; // 64MB safety cap

struct MetadataEntry
{
    int32_t metadataId;
    char providerName[256];
    char eventName[256];
};

struct NettraceParserState
{
    struct MetadataEntry metadata[MAX_METADATA_ENTRIES];
    int metadataCount;
    EventPipeCounterCallback callback;
    void* context;
    bool stopRequested;
    char metricsSessionId[64]; // SessionId sent to System.Diagnostics.Metrics
};

//--------------------------------------------------------------------
// Helper: Read exactly n bytes from fd, tracking stream position
//--------------------------------------------------------------------
static int read_exact(int fd, void* buf, size_t len)
{
    size_t total = 0;
    while (total < len)
    {
        ssize_t n = read(fd, (char*)buf + total, len - total);
        if (n <= 0)
        {
            if (n == 0) return -1; // EOF
            if (errno == EINTR) continue;
            return -1;
        }
        total += n;
    }
    return 0;
}

// Wrapper that also advances a stream position counter
static int read_exact_tracked(int fd, void* buf, size_t len, size_t* streamPos)
{
    int rc = read_exact(fd, buf, len);
    if (rc == 0 && streamPos) *streamPos += len;
    return rc;
}

//--------------------------------------------------------------------
// Helper: Skip n bytes from fd, tracking stream position
//--------------------------------------------------------------------
static int skip_bytes(int fd, size_t len)
{
    char buf[4096];
    while (len > 0)
    {
        size_t toRead = len < sizeof(buf) ? len : sizeof(buf);
        if (read_exact(fd, buf, toRead) != 0) return -1;
        len -= toRead;
    }
    return 0;
}

static int skip_bytes_tracked(int fd, size_t len, size_t* streamPos)
{
    size_t origLen = len;
    char buf[4096];
    while (len > 0)
    {
        size_t toRead = len < sizeof(buf) ? len : sizeof(buf);
        if (read_exact(fd, buf, toRead) != 0) return -1;
        len -= toRead;
    }
    if (streamPos) *streamPos += origLen;
    return 0;
}

//--------------------------------------------------------------------
// Helper: Read a length-prefixed nettrace string (serialized as
// int32 length in chars, followed by UTF-16LE chars).
// Converts to UTF-8 (ASCII subset) in outBuf.
//--------------------------------------------------------------------
static int read_nettrace_string(const uint8_t* data, size_t dataLen, size_t* offset, char* outBuf, size_t outBufSize)
{
    if (*offset + 4 > dataLen) return -1;
    int32_t charCount;
    memcpy(&charCount, data + *offset, 4);
    *offset += 4;

    if (charCount <= 0)
    {
        if (outBufSize > 0) outBuf[0] = '\0';
        return 0;
    }

    // Guard against overflow on 32-bit: cap charCount to half the max buffer
    if ((uint32_t)charCount > dataLen / 2)
        return -1;

    size_t byteCount = (size_t)charCount * 2;
    if (*offset + byteCount > dataLen) return -1;

    // Convert UTF-16LE to ASCII (sufficient for counter/provider names)
    size_t j = 0;
    for (int32_t i = 0; i < charCount && outBufSize > 1 && j < outBufSize - 1; i++)
    {
        uint16_t wc;
        memcpy(&wc, data + *offset + i * 2, 2);
        if (wc == 0) break; // null terminator
        outBuf[j++] = (wc < 128) ? (char)wc : '?';
    }
    if (outBufSize > 0) outBuf[j] = '\0';
    *offset += byteCount;
    return 0;
}

//--------------------------------------------------------------------
// Helper: Read a null-terminated UTF-16LE string (used in event payloads).
//--------------------------------------------------------------------
static int read_null_terminated_utf16(const uint8_t* data, size_t dataLen, size_t* offset, char* outBuf, size_t outBufSize)
{
    size_t j = 0;
    bool terminated = false;
    while (*offset + 2 <= dataLen)
    {
        uint16_t wc;
        memcpy(&wc, data + *offset, 2);
        *offset += 2;
        if (wc == 0)
        {
            terminated = true;
            break;
        }
        if (outBufSize > 1 && j < outBufSize - 1)
            outBuf[j++] = (wc < 128) ? (char)wc : '?';
    }
    if (outBufSize > 0) outBuf[j] = '\0';
    return terminated ? 0 : -1;
}

//--------------------------------------------------------------------
// Parse event payload that is an EventCounters event.
//
// EventCounters payloads contain a self-describing map.
// The payload structure for EventCounters event is:
//   - string Name (of the payload map, typically "EventCounters")
//   - int32 Count (number of items, always 1 for counters)
//   - For each item: a key-value map encoded as EventSource payload
//
// The inner map has fields like:
//   Name (string), DisplayName (string), Mean (double),
//   Increment (double), CounterType (string: "Mean" or "Sum")
//
// We use a simplified parsing approach that scans for known
// key-value patterns in the binary payload.
//--------------------------------------------------------------------
static bool parse_event_counter_payload(const uint8_t* payload, size_t payloadLen,
                                         const char* providerName,
                                         struct EventPipeCounterValue* result)
{
    // EventCounters payload is flat fields with null-terminated UTF-16LE strings.
    // Two layouts exist:
    //
    // CounterPayload (Gauge, CounterType="Mean"):
    //   1. Name (NULW)  2. DisplayName (NULW)  3. Mean (f64)
    //   4. StandardDeviation (f64)  5. Count (i32)  6. Min (f64)  7. Max (f64)
    //   8. IntervalSec (f32)  9. Series (NULW)  10. CounterType (NULW)
    //   11. Metadata (NULW)  12. DisplayUnits (NULW)
    //
    // IncrementingCounterPayload (Sum, CounterType="Sum"):
    //   1. Name (NULW)  2. DisplayName (NULW)  3. DisplayRateTimeScale (NULW)
    //   4. Increment (f64)  5. IntervalSec (f32)  6. Metadata (NULW)
    //   7. Series (NULW)  8. CounterType (NULW)  9. DisplayUnits (NULW)

    memset(result, 0, sizeof(*result));
    strncpy(result->providerName, providerName, sizeof(result->providerName) - 1);

    size_t pos = 0;

    // 1. Name (null-terminated UTF-16LE)
    if (read_null_terminated_utf16(payload, payloadLen, &pos, result->counterName, sizeof(result->counterName)) != 0)
        return false;

    // 2. DisplayName — skip
    char displayName[256] = {0};
    if (read_null_terminated_utf16(payload, payloadLen, &pos, displayName, sizeof(displayName)) != 0)
        return false;

    // Try gauge layout first (CounterPayload):
    //   Name, DisplayName, Mean(f64), StdDev(f64), Count(i32), Min(f64), Max(f64),
    //   IntervalSec(f32), Series(NULW), CounterType(NULW), Metadata(NULW), DisplayUnits(NULW)
    // Then try incrementing layout (IncrementingCounterPayload):
    //   Name, DisplayName, DisplayRateTimeScale(NULW), Increment(f64),
    //   IntervalSec(f32), Metadata(NULW), Series(NULW), CounterType(NULW), DisplayUnits(NULW)

    // Try gauge layout: 40 bytes of numerics after DisplayName
    size_t gaugeNumEnd = pos + 8 + 8 + 4 + 8 + 8 + 4; // Mean+StdDev+Count+Min+Max+IntervalSec
    if (gaugeNumEnd <= payloadLen)
    {
        // Parse gauge layout
        double mean;
        memcpy(&mean, payload + pos, 8);

        size_t tryPos = gaugeNumEnd;
        char series[64] = {0};
        char counterType[64] = {0};
        if (read_null_terminated_utf16(payload, payloadLen, &tryPos, series, sizeof(series)) == 0 &&
            read_null_terminated_utf16(payload, payloadLen, &tryPos, counterType, sizeof(counterType)) == 0)
        {
            if (strcasecmp(counterType, "Mean") == 0)
            {
                result->value = mean;
                strncpy(result->counterType, counterType, sizeof(result->counterType) - 1);
                return true;
            }
        }
    }

    // Try incrementing layout
    {
        size_t tryPos = pos;
        char displayRateTimeScale[64] = {0};
        if (read_null_terminated_utf16(payload, payloadLen, &tryPos, displayRateTimeScale, sizeof(displayRateTimeScale)) == 0 &&
            tryPos + 12 <= payloadLen) // Increment(8) + IntervalSec(4)
        {
            double increment;
            memcpy(&increment, payload + tryPos, 8);
            tryPos += 8 + 4; // skip Increment + IntervalSec

            char metadata[64] = {0};
            char series[64] = {0};
            char counterType[64] = {0};
            if (read_null_terminated_utf16(payload, payloadLen, &tryPos, metadata, sizeof(metadata)) == 0 &&
                read_null_terminated_utf16(payload, payloadLen, &tryPos, series, sizeof(series)) == 0 &&
                read_null_terminated_utf16(payload, payloadLen, &tryPos, counterType, sizeof(counterType)) == 0 &&
                strcasecmp(counterType, "Sum") == 0)
            {
                result->value = increment;
                strncpy(result->counterType, counterType, sizeof(result->counterType) - 1);
                return true;
            }
        }
    }

    // Fallback: couldn't determine type, but we have the name
    return result->counterName[0] != '\0';
}

//--------------------------------------------------------------------
// Parse a System.Diagnostics.Metrics event payload.
//
// Metrics events from the System.Diagnostics.Metrics EventSource use
// manifest-based EventSource format where string fields are null-terminated
// UTF-16LE (matching ETW EventData layout). Numeric fields are inline LE.
//
// Event types and their fields (from MetricsEventSource.cs):
//
// GaugeValuePublished:
//   [0] SessionId (string), [1] MeterName (string),
//   [2] MeterVersion (string), [3] InstrumentName (string),
//   [4] Unit (string), [5] Tags (string), [6] LastValue (string)
//
// CounterRateValuePublished:
//   [0] SessionId (string), [1] MeterName (string),
//   [2] MeterVersion (string), [3] InstrumentName (string),
//   [4] Unit (string), [5] Tags (string), [6] Rate (string),
//   [7] Value (string)   -- V1+
//
// UpDownCounterRateValuePublished:
//   [0] SessionId (string), [1] MeterName (string),
//   [2] MeterVersion (string), [3] InstrumentName (string),
//   [4] Unit (string), [5] Tags (string), [6] Rate (string),
//   [7] Value (string)   -- V1+
//
// HistogramValuePublished:
//   [0] SessionId (string), [1] MeterName (string),
//   [2] MeterVersion (string), [3] InstrumentName (string),
//   [4] Unit (string), [5] Tags (string),
//   [6] Quantiles (string, format: "0.5=1.234;0.95=2.345;0.99=5.678"),
//   [7] Count (int32) -- V1+, [8] Sum (double) -- V1+
//--------------------------------------------------------------------
static bool parse_metrics_event_payload(const char* eventName,
                                        const uint8_t* payload, size_t payloadLen,
                                        const char* metricsSessionId,
                                        struct EventPipeCounterValue* result)
{
    memset(result, 0, sizeof(*result));
    size_t pos = 0;

    // MetricsEventSource uses manifest-based EventSource, so string fields
    // are null-terminated UTF-16LE (no length prefix), matching ETW EventData layout.

    // Field 0: SessionId
    char sessionId[128] = {0};
    if (read_null_terminated_utf16(payload, payloadLen, &pos, sessionId, sizeof(sessionId)) != 0)
        return false;

    // Verify session ID matches ours
    if (metricsSessionId[0] != '\0' && strcmp(sessionId, metricsSessionId) != 0)
        return false;

    // Field 1: MeterName (this is the "provider" for Metrics)
    if (read_null_terminated_utf16(payload, payloadLen, &pos, result->providerName, sizeof(result->providerName)) != 0)
        return false;

    // Field 2: MeterVersion — skip
    char meterVersion[64] = {0};
    if (read_null_terminated_utf16(payload, payloadLen, &pos, meterVersion, sizeof(meterVersion)) != 0)
        return false;

    // Field 3: InstrumentName (this is the "counter" for Metrics)
    if (read_null_terminated_utf16(payload, payloadLen, &pos, result->counterName, sizeof(result->counterName)) != 0)
        return false;

    // Field 4: Unit — skip
    char unit[64] = {0};
    if (read_null_terminated_utf16(payload, payloadLen, &pos, unit, sizeof(unit)) != 0)
        return false;

    // Field 5: Tags — skip
    char tags[256] = {0};
    if (read_null_terminated_utf16(payload, payloadLen, &pos, tags, sizeof(tags)) != 0)
        return false;

    if (strcasecmp(eventName, "GaugeValuePublished") == 0)
    {
        // Field 6: LastValue (string)
        char lastValueText[64] = {0};
        if (read_null_terminated_utf16(payload, payloadLen, &pos, lastValueText, sizeof(lastValueText)) != 0)
            return false;

        if (lastValueText[0] == '\0')
            return false;

        char* endPtr = NULL;
        double val = strtod(lastValueText, &endPtr);
        if (endPtr == lastValueText || *endPtr != '\0' || !isfinite(val))
            return false;
        result->value = val;
        strncpy(result->counterType, "Gauge", sizeof(result->counterType) - 1);
        return true;
    }
    else if (strcasecmp(eventName, "CounterRateValuePublished") == 0)
    {
        // Field 6: Rate (string)
        char rateText[64] = {0};
        if (read_null_terminated_utf16(payload, payloadLen, &pos, rateText, sizeof(rateText)) != 0)
            return false;

        if (rateText[0] == '\0')
            return false;

        char* endPtr = NULL;
        double val = strtod(rateText, &endPtr);
        if (endPtr == rateText || *endPtr != '\0' || !isfinite(val))
            return false;
        result->value = val;
        strncpy(result->counterType, "Rate", sizeof(result->counterType) - 1);
        return true;
    }
    else if (strcasecmp(eventName, "UpDownCounterRateValuePublished") == 0)
    {
        // Field 6: Rate (string) — skip to get value
        char rateText[64] = {0};
        if (read_null_terminated_utf16(payload, payloadLen, &pos, rateText, sizeof(rateText)) != 0)
            return false;

        // Field 7: Value (string) — V1+, the absolute value
        char valueText[64] = {0};
        if (read_null_terminated_utf16(payload, payloadLen, &pos, valueText, sizeof(valueText)) == 0 && valueText[0] != '\0')
        {
            char* endPtr = NULL;
            double val = strtod(valueText, &endPtr);
            if (endPtr == valueText || *endPtr != '\0' || !isfinite(val))
                return false;
            result->value = val;
        }
        else if (rateText[0] != '\0')
        {
            char* endPtr = NULL;
            double val = strtod(rateText, &endPtr);
            if (endPtr == rateText || *endPtr != '\0' || !isfinite(val))
                return false;
            result->value = val;
        }
        else
        {
            return false;
        }

        strncpy(result->counterType, "UpDownCounter", sizeof(result->counterType) - 1);
        return true;
    }
    else if (strcasecmp(eventName, "HistogramValuePublished") == 0)
    {
        // Field 6: Quantiles (string, format: "0.5=1.234;0.95=2.345;0.99=5.678")
        char quantilesText[512] = {0};
        if (read_null_terminated_utf16(payload, payloadLen, &pos, quantilesText, sizeof(quantilesText)) != 0)
            return false;

        if (quantilesText[0] == '\0')
            return false;

        // Store raw quantiles for the callback to select the right percentile
        strncpy(result->quantiles, quantilesText, sizeof(result->quantiles) - 1);

        // Default: pick p50 (0.5) as the value for initial display/logging
        double p50 = 0.0;
        bool foundP50 = false;
        double firstValue = 0.0;
        bool foundAny = false;

        // Parse without modifying stored quantiles — use a local copy
        char parseBuf[512];
        strncpy(parseBuf, quantilesText, sizeof(parseBuf) - 1);
        parseBuf[sizeof(parseBuf) - 1] = '\0';

        char* savePtr = NULL;
        char* token = strtok_r(parseBuf, ";", &savePtr);
        while (token != NULL)
        {
            char* eq = strchr(token, '=');
            if (eq != NULL)
            {
                *eq = '\0';
                char* endKey = NULL;
                char* endVal = NULL;
                double qKey = strtod(token, &endKey);
                double qVal = strtod(eq + 1, &endVal);
                if (endKey == token || *endKey != '\0' || !isfinite(qKey) ||
                    endVal == (eq + 1) || *endVal != '\0' || !isfinite(qVal))
                {
                    token = strtok_r(NULL, ";", &savePtr);
                    continue;
                }

                if (!foundAny)
                {
                    firstValue = qVal;
                    foundAny = true;
                }

                if (fabs(qKey - 0.5) < 0.001)
                {
                    p50 = qVal;
                    foundP50 = true;
                }
            }
            token = strtok_r(NULL, ";", &savePtr);
        }
        if (!foundAny)
            return false;

        result->value = foundP50 ? p50 : firstValue;
        strncpy(result->counterType, "Histogram", sizeof(result->counterType) - 1);
        return true;
    }

    return false;
}

// Forward declarations
static struct MetadataEntry* find_metadata(struct NettraceParserState* state, int32_t metadataId);

//--------------------------------------------------------------------
// Helper: Read a varuint32 from a buffer
//--------------------------------------------------------------------
static uint32_t read_varuint32(const uint8_t* data, size_t dataLen, size_t* pos)
{
    uint32_t result = 0;
    int shift = 0;
    while (*pos < dataLen && shift < 35)
    {
        uint8_t b = data[(*pos)++];
        result |= (uint32_t)(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return result;
        shift += 7;
    }
    return result;
}

//--------------------------------------------------------------------
// Helper: Read a varuint64 from a buffer
//--------------------------------------------------------------------
static uint64_t read_varuint64(const uint8_t* data, size_t dataLen, size_t* pos)
{
    uint64_t result = 0;
    int shift = 0;
    while (*pos < dataLen && shift < 70)
    {
        uint8_t b = data[(*pos)++];
        result |= (uint64_t)(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return result;
        shift += 7;
    }
    return result;
}

//--------------------------------------------------------------------
// Parse events from a V4/V5 block (MetadataBlock or EventBlock).
//
// Block internal format (shared by MetadataBlock and EventBlock):
//   int16 headerSize (including the headerSize field itself)
//   int16 flags (bit 0 = HeaderCompression)
//   <remaining header bytes: timestamps, etc.>
//   Then events, each with:
//     If compressed (flags & 1):
//       byte perEventFlags
//       [fields present based on perEventFlags bits]
//       varuint64 timestampDelta (always)
//       [more optional fields]
//     If not compressed (fixed V4 format):
//       int32 eventSize, int32 metadataId, int32 seqNum,
//       int64 threadId, int64 captureThreadId, int32 procNum,
//       int32 stackId, int64 timestamp, guid activityId,
//       guid relatedActivityId, int32 payloadSize
//     Then: payloadSize bytes of payload
//
// For each event, the callback is invoked with the metadataId,
// payload pointer, and payload size.
//--------------------------------------------------------------------
typedef void (*BlockEventCallback)(uint32_t metadataId, const uint8_t* payload,
                                   uint32_t payloadSize, struct NettraceParserState* state);

static int parse_block_events(int fd, size_t blockLen, struct NettraceParserState* state,
                              BlockEventCallback eventCb)
{
    if (blockLen == 0) return 0;
    if (blockLen > MAX_NETTRACE_BLOCK_SIZE) return -1;

    uint8_t* blockData = (uint8_t*)malloc(blockLen);
    if (!blockData) return -1;

    if (read_exact(fd, blockData, blockLen) != 0)
    {
        free(blockData);
        return -1;
    }

    size_t pos = 0;

    // Read block header
    if (pos + 4 > blockLen) { free(blockData); return 0; }
    uint16_t headerSize;
    memcpy(&headerSize, blockData + pos, 2);
    pos += 2;
    uint16_t blockFlags;
    memcpy(&blockFlags, blockData + pos, 2);
    pos += 2;

    bool useCompression = (blockFlags & 0x01) != 0;

    Trace("parse_block_events: headerSize=%u blockFlags=0x%04x compressed=%d blockLen=%zu",
          headerSize, blockFlags, useCompression, blockLen);

    // Skip rest of header (headerSize includes the 2-byte headerSize field itself)
    if (headerSize > 4)
    {
        size_t skip = headerSize - 4;
        if (pos + skip > blockLen) { free(blockData); return 0; }
        pos += skip;
    }

    // Parse events
    // State that persists across compressed events
    uint32_t lastMetadataId = 0;
    int32_t lastPayloadSize = 0;

    while (pos < blockLen && !state->stopRequested)
    {
        uint32_t metadataId = 0;
        uint32_t payloadSize = 0;

        if (useCompression)
        {
            // Compressed event header per PerfView spec
            if (pos >= blockLen) break;
            uint8_t perEventFlags = blockData[pos++];


            // MetadataId (bit 0)
            if (perEventFlags & 0x01)
            {
                metadataId = read_varuint32(blockData, blockLen, &pos);
                lastMetadataId = metadataId;
            }
            else
            {
                metadataId = lastMetadataId;
            }

            // CaptureThreadAndSequence (bit 1) — skip
            if (perEventFlags & 0x02)
            {
                read_varuint32(blockData, blockLen, &pos); // SequenceNumber delta
                read_varuint64(blockData, blockLen, &pos); // CaptureThreadId
                read_varuint32(blockData, blockLen, &pos); // ProcessorNumber
            }

            // ThreadId (bit 2) — skip
            if (perEventFlags & 0x04)
            {
                read_varuint64(blockData, blockLen, &pos);
            }

            // StackId (bit 3) — skip
            if (perEventFlags & 0x08)
            {
                read_varuint32(blockData, blockLen, &pos);
            }

            // Timestamp delta (always present)
            read_varuint64(blockData, blockLen, &pos);

            // ActivityId (bit 4) — skip 16 bytes
            if (perEventFlags & 0x10)
            {
                if (pos + 16 > blockLen) break;
                pos += 16;
            }

            // RelatedActivityId (bit 5) — skip 16 bytes
            if (perEventFlags & 0x20)
            {
                if (pos + 16 > blockLen) break;
                pos += 16;
            }

            // Sorted = bit 6 (no data to read)

            // DataLength (bit 7)
            if (perEventFlags & 0x80)
            {
                payloadSize = read_varuint32(blockData, blockLen, &pos);
                lastPayloadSize = payloadSize;
            }
            else
            {
                payloadSize = lastPayloadSize;
            }
        }
        else
        {
            // Non-compressed V4 event header (fixed layout):
            //   int32 eventSize, int32 metadataId, int32 seqNum,
            //   int64 threadId, int64 captureThreadId, int32 procNum,
            //   int32 stackId, int64 timestamp, guid activityId,
            //   guid relatedActivityId, int32 payloadSize
            // Total: 4+4+4+8+8+4+4+8+16+16+4 = 80 bytes
            if (pos + 80 > blockLen) break;

            int32_t eventSize;
            memcpy(&eventSize, blockData + pos, 4);
            pos += 4;

            int32_t mid;
            memcpy(&mid, blockData + pos, 4);
            metadataId = mid & 0x7FFFFFFF;
            pos += 4;

            pos += 4;  // SequenceNumber
            pos += 8;  // ThreadId
            pos += 8;  // CaptureThreadId
            pos += 4;  // ProcessorNumber
            pos += 4;  // StackId
            pos += 8;  // Timestamp
            pos += 16; // ActivityId
            pos += 16; // RelatedActivityId

            int32_t ps;
            memcpy(&ps, blockData + pos, 4);
            if (ps < 0) break;
            payloadSize = (uint32_t)ps;
            pos += 4;
        }

        if (payloadSize > blockLen - pos)
        {
            break;
        }

        Trace("parse_block_events: event metadataId=%u payloadSize=%u pos=%zu", metadataId, payloadSize, pos);
        eventCb(metadataId, blockData + pos, payloadSize, state);

        pos += payloadSize;

        // Align to 4 bytes (only for non-compressed format)
        if (!useCompression)
        {
            size_t align = (4 - (payloadSize % 4)) % 4;
            if (pos + align > blockLen) break;
            pos += align;
        }
    }

    free(blockData);
    return 0;
}

//--------------------------------------------------------------------
// Callback: process a metadata event payload
//--------------------------------------------------------------------
static void metadata_event_callback(uint32_t metadataId, const uint8_t* payload,
                                    uint32_t payloadSize, struct NettraceParserState* state)
{
    (void)metadataId; // The metadataId in the event header is 0 for metadata events

    // Metadata payload format (V4/V5):
    //   int32 MetaDataId (the ID being defined)
    //   NullTerminatedUTF16 ProviderName
    //   int32 EventId
    //   NullTerminatedUTF16 EventName
    //   ... (keywords, version, level, params — we skip)

    size_t pos = 0;
    if (pos + 4 > payloadSize) return;

    int32_t definedMetaId;
    memcpy(&definedMetaId, payload + pos, 4);
    pos += 4;

    char providerName[256] = {0};
    if (read_null_terminated_utf16(payload, payloadSize, &pos, providerName, sizeof(providerName)) != 0)
        return;

    if (pos + 4 > payloadSize) return;
    pos += 4; // EventId

    char eventName[256] = {0};
    if (read_null_terminated_utf16(payload, payloadSize, &pos, eventName, sizeof(eventName)) != 0)
        return;

    if (state->metadataCount < MAX_METADATA_ENTRIES)
    {
        struct MetadataEntry* entry = &state->metadata[state->metadataCount];
        entry->metadataId = definedMetaId;
        strncpy(entry->providerName, providerName, sizeof(entry->providerName) - 1);
        entry->providerName[sizeof(entry->providerName) - 1] = '\0';
        strncpy(entry->eventName, eventName, sizeof(entry->eventName) - 1);
        entry->eventName[sizeof(entry->eventName) - 1] = '\0';
        state->metadataCount++;
        Trace("metadata_event_callback: registered metaId=%d provider='%s' event='%s'",
              definedMetaId, providerName, eventName);
    }
}

//--------------------------------------------------------------------
// Callback: process an event payload (look for EventCounters and
// System.Diagnostics.Metrics events)
//--------------------------------------------------------------------
static void event_payload_callback(uint32_t metadataId, const uint8_t* payload,
                                   uint32_t payloadSize, struct NettraceParserState* state)
{
    if (payloadSize == 0 || metadataId == 0) return;

    struct MetadataEntry* meta = find_metadata(state, metadataId);
    if (meta == NULL) return;

    // Check for legacy EventCounters events
    if (strcasecmp(meta->eventName, "EventCounters") == 0)
    {
        struct EventPipeCounterValue counterValue;
        if (parse_event_counter_payload(payload, payloadSize, meta->providerName, &counterValue))
        {
            Trace("event_payload_callback: Counter: %s:%s = %.4f (type=%s)",
                  counterValue.providerName, counterValue.counterName,
                  counterValue.value, counterValue.counterType);
            if (!state->callback(&counterValue, state->context))
            {
                state->stopRequested = true;
            }
        }
        return;
    }

    // Check for System.Diagnostics.Metrics events
    if (strcasecmp(meta->providerName, METRICS_PROVIDER_NAME) == 0 &&
        (strcasecmp(meta->eventName, "GaugeValuePublished") == 0 ||
         strcasecmp(meta->eventName, "CounterRateValuePublished") == 0 ||
         strcasecmp(meta->eventName, "UpDownCounterRateValuePublished") == 0 ||
         strcasecmp(meta->eventName, "HistogramValuePublished") == 0))
    {
        struct EventPipeCounterValue counterValue;
        if (parse_metrics_event_payload(meta->eventName, payload, payloadSize,
                                         state->metricsSessionId, &counterValue))
        {
            Trace("event_payload_callback: Metrics: %s:%s = %.4f (type=%s)",
                  counterValue.providerName, counterValue.counterName,
                  counterValue.value, counterValue.counterType);
            if (!state->callback(&counterValue, state->context))
            {
                state->stopRequested = true;
            }
        }
    }
}

//--------------------------------------------------------------------
// Parse a metadata block (V4/V5 format)
//--------------------------------------------------------------------
static int parse_metadata_block(int fd, size_t blockLen, struct NettraceParserState* state)
{
    return parse_block_events(fd, blockLen, state, metadata_event_callback);
}

//--------------------------------------------------------------------
// Find metadata entry by ID
//--------------------------------------------------------------------
static struct MetadataEntry* find_metadata(struct NettraceParserState* state, int32_t metadataId)
{
    for (int i = 0; i < state->metadataCount; i++)
    {
        if (state->metadata[i].metadataId == metadataId)
            return &state->metadata[i];
    }
    return NULL;
}

//--------------------------------------------------------------------
// Parse an event block (V4/V5 format)
//--------------------------------------------------------------------
static int parse_event_block(int fd, size_t blockLen, struct NettraceParserState* state)
{
    return parse_block_events(fd, blockLen, state, event_payload_callback);
}

//--------------------------------------------------------------------
// Read a serialized string (length-prefixed, in bytes)
//--------------------------------------------------------------------
static int read_serialized_string(int fd, char* buf, size_t bufSize)
{
    int32_t len;
    if (read_exact(fd, &len, 4) != 0) return -1;
    if (len <= 0 || (size_t)len + 1 > bufSize)
    {
        if (len > 0) skip_bytes(fd, len);
        buf[0] = '\0';
        return 0;
    }
    if (read_exact(fd, buf, len) != 0) return -1;
    buf[len] = '\0';
    return 0;
}

static int read_serialized_string_tracked(int fd, char* buf, size_t bufSize, size_t* streamPos)
{
    int32_t len;
    if (read_exact_tracked(fd, &len, 4, streamPos) != 0) return -1;
    if (len <= 0 || (size_t)len + 1 > bufSize)
    {
        if (len > 0) skip_bytes_tracked(fd, len, streamPos);
        buf[0] = '\0';
        return 0;
    }
    if (read_exact_tracked(fd, buf, len, streamPos) != 0) return -1;
    buf[len] = '\0';
    return 0;
}

//--------------------------------------------------------------------
// Per-provider configuration for EventPipe subscription
//--------------------------------------------------------------------
struct EventPipeProviderConfig
{
    const char* providerName;
    uint64_t keywords;
    uint32_t logLevel;
    const char* filterData;  // argument string (e.g., "EventCounterIntervalSec=1")
};

//--------------------------------------------------------------------
// Build the EventPipe CollectTracing2 IPC command
//--------------------------------------------------------------------
static int build_collect_tracing2_command(
    struct EventPipeProviderConfig* providerConfigs,
    int providerCount,
    uint8_t** outBuffer,
    size_t* outSize)
{
    // Calculate payload size
    // Payload layout:
    //   uint32 circularBufferMB
    //   uint32 format (1 = NetTrace)
    //   bool   requestRundown (false)
    //   uint32 providerCount
    //   For each provider:
    //     uint64 keywords
    //     uint32 logLevel (4 = Informational)
    //     uint32 providerNameLen (in wchars including null)
    //     wchar[] providerName
    //     uint32 argsLen (in wchars including null)
    //     wchar[] arguments

    size_t payloadSize = 0;
    payloadSize += 4; // circularBufferMB
    payloadSize += 4; // format
    payloadSize += 1; // requestRundown
    payloadSize += 4; // provider count

    for (int i = 0; i < providerCount; i++)
    {
        const char* filterData = providerConfigs[i].filterData ? providerConfigs[i].filterData : "";
        payloadSize += 8; // keywords
        payloadSize += 4; // logLevel
        payloadSize += 4; // provider name length
        payloadSize += (strlen(providerConfigs[i].providerName) + 1) * 2; // provider name UTF-16
        payloadSize += 4; // args length
        payloadSize += (strlen(filterData) + 1) * 2; // args UTF-16
    }

    size_t totalSize = sizeof(struct IpcHeader) + payloadSize;
    if (totalSize > UINT16_MAX)
    {
        return -1;
    }
    uint8_t* buffer = (uint8_t*)calloc(1, totalSize);
    if (!buffer) return -1;

    // Header
    struct IpcHeader* header = (struct IpcHeader*)buffer;
    memcpy(header->Magic, "DOTNET_IPC_V1", 14);
    header->Size = (uint16_t)totalSize;
    header->CommandSet = 0x02; // EventPipe
    header->CommandId = 0x03;  // CollectTracing2
    header->Reserved = 0;

    uint8_t* cur = buffer + sizeof(struct IpcHeader);

    // circularBufferMB
    uint32_t circularBufferMB = 256;
    memcpy(cur, &circularBufferMB, 4); cur += 4;

    // format (1 = NetTrace)
    uint32_t format = 1;
    memcpy(cur, &format, 4); cur += 4;

    // requestRundown (false)
    uint8_t requestRundown = 0;
    *cur++ = requestRundown;

    // provider count
    uint32_t provCount = (uint32_t)providerCount;
    memcpy(cur, &provCount, 4); cur += 4;

    for (int i = 0; i < providerCount; i++)
    {
        const char* filterData = providerConfigs[i].filterData ? providerConfigs[i].filterData : "";

        // keywords
        memcpy(cur, &providerConfigs[i].keywords, 8); cur += 8;

        // logLevel
        memcpy(cur, &providerConfigs[i].logLevel, 4); cur += 4;

        // provider name (length-prefixed UTF-16)
        uint32_t nameLen = (uint32_t)(strlen(providerConfigs[i].providerName) + 1);
        memcpy(cur, &nameLen, 4); cur += 4;
        for (uint32_t j = 0; j < nameLen; j++)
        {
            uint16_t wc = (j < strlen(providerConfigs[i].providerName)) ? (uint16_t)(unsigned char)providerConfigs[i].providerName[j] : 0;
            memcpy(cur, &wc, 2); cur += 2;
        }

        // arguments (length-prefixed UTF-16)
        uint32_t argsLen = (uint32_t)(strlen(filterData) + 1);
        memcpy(cur, &argsLen, 4); cur += 4;
        for (uint32_t j = 0; j < argsLen; j++)
        {
            uint16_t wc = (j < strlen(filterData)) ? (uint16_t)(unsigned char)filterData[j] : 0;
            memcpy(cur, &wc, 2); cur += 2;
        }
    }

    *outBuffer = buffer;
    *outSize = totalSize;
    return 0;
}

static void set_session_fd(int* sessionFd, pthread_mutex_t* sessionFdMutex, int value)
{
    if (!sessionFd)
    {
        return;
    }

    if (sessionFdMutex)
    {
        pthread_mutex_lock(sessionFdMutex);
    }

    *sessionFd = value;

    if (sessionFdMutex)
    {
        pthread_mutex_unlock(sessionFdMutex);
    }
}

//--------------------------------------------------------------------
//
// StartEventPipeCounterSession
//
//--------------------------------------------------------------------
int StartEventPipeCounterSession(
    const char* socketName,
    const char** providers,
    int providerCount,
    int intervalSeconds,
    EventPipeCounterCallback callback,
    void* context,
    uint64_t* sessionId,
    int* sessionFd,
    pthread_mutex_t* sessionFdMutex)
{
    int fd = -1;
    struct sockaddr_un addr = {0};

    set_session_fd(sessionFd, sessionFdMutex, -1);
    uint8_t* cmdBuffer = NULL;
    size_t cmdSize = 0;

    Trace("StartEventPipeCounterSession: Enter");

    if (intervalSeconds < 1) intervalSeconds = 1;

    // Generate a unique session ID for System.Diagnostics.Metrics filtering
    char metricsSessionId[64];
    {
        // Simple pseudo-unique ID based on pid + time (good enough for session filtering)
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        snprintf(metricsSessionId, sizeof(metricsSessionId), "%d-%ld-%ld",
                 getpid(), (long)ts.tv_sec, (long)ts.tv_nsec);
    }

    // Build provider configs: EventCounter providers + System.Diagnostics.Metrics
    // We subscribe to both protocols so we get events from either API.
    // Max providers: user providers (for EventCounters) + 1 (for Metrics) + user providers again (for Metrics meter names)
    int totalProviderCount = providerCount + 1; // user EventCounter providers + System.Diagnostics.Metrics
    struct EventPipeProviderConfig* providerConfigs = (struct EventPipeProviderConfig*)calloc(totalProviderCount, sizeof(struct EventPipeProviderConfig));
    if (!providerConfigs)
    {
        return -1;
    }

    // EventCounter argument string
    char eventCounterArgs[64];
    snprintf(eventCounterArgs, sizeof(eventCounterArgs), "EventCounterIntervalSec=%d", intervalSeconds);

    // Add EventCounter providers (subscribe to each user provider for legacy EventCounters)
    for (int i = 0; i < providerCount; i++)
    {
        providerConfigs[i].providerName = providers[i];
        providerConfigs[i].keywords = 0; // all keywords
        providerConfigs[i].logLevel = 4; // Informational
        providerConfigs[i].filterData = eventCounterArgs;
    }

    // Build the Metrics filter data string: SessionId=X&Metrics=provider1,provider2&RefreshInterval=N&MaxTimeSeries=1000&MaxHistograms=20&ClientId=Y
    // The Metrics provider needs all meter names (same as the user provider names)
    char metricsFilterData[2048]; // buffer for filter string
    {
        char meterNames[1024] = {0};
        size_t meterOff = 0;
        bool meterTruncated = false;
        for (int i = 0; i < providerCount; i++)
        {
            if (meterOff > 0 && meterOff < sizeof(meterNames) - 1)
            {
                meterNames[meterOff++] = ',';
            }
            else if (meterOff >= sizeof(meterNames) - 1)
            {
                meterTruncated = true;
            }
            size_t plen = strlen(providers[i]);
            if (meterOff + plen < sizeof(meterNames) - 1)
            {
                memcpy(meterNames + meterOff, providers[i], plen);
                meterOff += plen;
            }
            else
            {
                meterTruncated = true;
            }
        }
        meterNames[meterOff] = '\0';

        if (meterTruncated)
        {
            Log(warn, "Metrics provider list truncated; some meters may be missing from the session filter.");
        }

        int written = snprintf(metricsFilterData, sizeof(metricsFilterData),
                               "SessionId=%s;Metrics=%s;RefreshInterval=%d;MaxTimeSeries=1000;MaxHistograms=20;ClientId=%s",
                               metricsSessionId, meterNames, intervalSeconds, metricsSessionId);
        if (written < 0 || (size_t)written >= sizeof(metricsFilterData))
        {
            Log(warn, "Metrics filter data truncated; session configuration may be incomplete.");
        }
    }

    // Add System.Diagnostics.Metrics provider
    providerConfigs[providerCount].providerName = METRICS_PROVIDER_NAME;
    providerConfigs[providerCount].keywords = METRICS_KEYWORD_TIMESERIES;
    providerConfigs[providerCount].logLevel = 4; // Informational
    providerConfigs[providerCount].filterData = metricsFilterData;

    Trace("StartEventPipeCounterSession: metricsSessionId='%s' filterData='%s'", metricsSessionId, metricsFilterData);

    // Connect to diagnostics socket
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
    {
        Trace("StartEventPipeCounterSession: Failed to create socket [%d]", errno);
        free(providerConfigs);
        return -1;
    }

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketName, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un)) == -1)
    {
        Trace("StartEventPipeCounterSession: Failed to connect [%d]", errno);
        Log(error, "Failed to connect to .NET diagnostics socket at %s [%d].", socketName, errno);
        set_session_fd(sessionFd, sessionFdMutex, -1);
        close(fd);
        free(providerConfigs);
        return -1;
    }

    // Build and send CollectTracing2 command
    if (build_collect_tracing2_command(providerConfigs, totalProviderCount, &cmdBuffer, &cmdSize) != 0)
    {
        Trace("StartEventPipeCounterSession: Failed to build command");
        set_session_fd(sessionFd, sessionFdMutex, -1);
        close(fd);
        free(providerConfigs);
        return -1;
    }
    free(providerConfigs);
    providerConfigs = NULL;

    if (send_all(fd, cmdBuffer, cmdSize) != 0)
    {
        Trace("StartEventPipeCounterSession: Failed to send command [%d]", errno);
        free(cmdBuffer);
        set_session_fd(sessionFd, sessionFdMutex, -1);
        close(fd);
        return -1;
    }
    free(cmdBuffer);

    // Read response header
    struct IpcHeader responseHeader;
    if (recv_all(fd, &responseHeader, sizeof(responseHeader)) != 0)
    {
        Trace("StartEventPipeCounterSession: Failed to read response header [%d]", errno);
        set_session_fd(sessionFd, sessionFdMutex, -1);
        close(fd);
        return -1;
    }

    // Check for success
    if (responseHeader.CommandSet != 0xFF || responseHeader.CommandId != 0x00)
    {
        Trace("StartEventPipeCounterSession: Server returned error (set=0x%02x, id=0x%02x)", responseHeader.CommandSet, responseHeader.CommandId);
        Log(error, "Failed to start EventPipe session. The target process may not support performance counter monitoring.");
        set_session_fd(sessionFd, sessionFdMutex, -1);
        close(fd);
        return -1;
    }

    // Read session ID from payload
    uint64_t sid = 0;
    if (responseHeader.Size > sizeof(responseHeader))
    {
        size_t payloadSize = responseHeader.Size - sizeof(responseHeader);
        if (payloadSize >= 8)
        {
            if (recv_all(fd, &sid, 8) != 0)
            {
                Trace("StartEventPipeCounterSession: Failed to read session ID");
                set_session_fd(sessionFd, sessionFdMutex, -1);
                close(fd);
                return -1;
            }
            payloadSize -= 8;
        }

        // Always consume remaining payload bytes to keep stream aligned.
        if (payloadSize > 0)
        {
            if (skip_bytes(fd, payloadSize) != 0)
            {
                Trace("StartEventPipeCounterSession: Failed to skip response payload");
                set_session_fd(sessionFd, sessionFdMutex, -1);
                close(fd);
                return -1;
            }
        }
    }

    *sessionId = sid;
    set_session_fd(sessionFd, sessionFdMutex, fd);
    Trace("StartEventPipeCounterSession: Session started (id=%lu)", (unsigned long)sid);

    // Now read the nettrace stream
    struct NettraceParserState state;
    memset(&state, 0, sizeof(state));
    state.callback = callback;
    state.context = context;
    state.stopRequested = false;
    strncpy(state.metricsSessionId, metricsSessionId, sizeof(state.metricsSessionId) - 1);

    // Read nettrace magic: "Nettrace" + serialization header
    char magicBuf[32];
    if (read_exact(fd, magicBuf, 8) != 0)
    {
        Trace("StartEventPipeCounterSession: Failed to read nettrace magic");
        set_session_fd(sessionFd, sessionFdMutex, -1);
        close(fd);
        return -1;
    }

    if (memcmp(magicBuf, NETTRACE_MAGIC, 8) != 0)
    {
        Trace("StartEventPipeCounterSession: Invalid nettrace magic");
        set_session_fd(sessionFd, sessionFdMutex, -1);
        close(fd);
        return -1;
    }

    // Read the "!FastSerialization.1" header string
    char serHeader[64] = {0};
    if (read_serialized_string(fd, serHeader, sizeof(serHeader)) != 0)
    {
        Trace("StartEventPipeCounterSession: Failed to read serialization header");
        set_session_fd(sessionFd, sessionFdMutex, -1);
        close(fd);
        return -1;
    }

    // Track stream position after the serialization header for alignment.
    // Position = 8 ("Nettrace") + 4 (string len) + strlen("!FastSerialization.1")
    size_t streamPos = 8 + 4 + strlen(serHeader);

    // Main parsing loop
    //
    // Per the FastSerialization spec (PerfView FastSerializationObjectParser),
    // each block in the nettrace stream has this format:
    //
    //   byte  BeginPrivateObject (5)        — outer object
    //   byte  BeginPrivateObject (5)        — inline type definition
    //   byte  NullReference (1)             — type-of-type (always null = "SerializationType")
    //   int32 version                       — SerializationType field
    //   int32 minimumReaderVersion          — SerializationType field
    //   string typeName                     — SerializationType field (int32 len + chars)
    //   byte  EndObject (6)                 — end of type definition
    //   <block data>                        — depends on typeName
    //   byte  EndObject (6)                 — end of outer object
    //
    // For Trace blocks: block data = 48 fixed bytes (version >= 3)
    // For other blocks: int32 blockSize + alignment_padding + blockSize bytes
    //   where alignment_padding = bytes needed to reach 4-byte alignment
    //
    // The stream ends with a NullReference (1) byte instead of BeginPrivateObject.
    //
    while (!state.stopRequested)
    {
        // Read outer tag
        uint8_t outerTag;
        if (read_exact_tracked(fd, &outerTag, 1, &streamPos) != 0)
        {
            Trace("StartEventPipeCounterSession: EOF/error on outer tag");
            break;
        }

        if (outerTag == FAST_SER_TAG_NULL_REF)
        {
            // End of stream
            Trace("StartEventPipeCounterSession: NullReference - end of stream");
            break;
        }

        if (outerTag != FAST_SER_TAG_BEGIN_OBJECT)
        {
            Trace("StartEventPipeCounterSession: unexpected outer tag=%d", outerTag);
            break;
        }

        // Read inline type definition: BeginPrivateObject(5) + NullReference(1)
        uint8_t typeDefTag;
        if (read_exact_tracked(fd, &typeDefTag, 1, &streamPos) != 0) break;
        if (typeDefTag != FAST_SER_TAG_BEGIN_OBJECT)
        {
            Trace("StartEventPipeCounterSession: expected BeginPrivateObject for type def, got %d", typeDefTag);
            break;
        }

        uint8_t nullRefTag;
        if (read_exact_tracked(fd, &nullRefTag, 1, &streamPos) != 0) break;
        if (nullRefTag != FAST_SER_TAG_NULL_REF)
        {
            Trace("StartEventPipeCounterSession: expected NullReference for type-of-type, got %d", nullRefTag);
            break;
        }

        // Read SerializationType fields: version, minVersion, typeName
        int32_t version, minVersion;
        if (read_exact_tracked(fd, &version, 4, &streamPos) != 0) break;
        if (read_exact_tracked(fd, &minVersion, 4, &streamPos) != 0) break;

        char typeName[256] = {0};
        if (read_serialized_string_tracked(fd, typeName, sizeof(typeName), &streamPos) != 0) break;

        // Read EndObject for type definition
        uint8_t endTypeTag;
        if (read_exact_tracked(fd, &endTypeTag, 1, &streamPos) != 0) break;

        Trace("StartEventPipeCounterSession: block type='%s' ver=%d minVer=%d", typeName, version, minVersion);

        // Read block data based on type name
        if (strcmp(typeName, TRACE_OBJECT_TAG) == 0)
        {
            // Trace block: fixed 48 bytes for version >= 3
            int traceSize = (version >= 3) ? 48 : 32;
            if (skip_bytes_tracked(fd, traceSize, &streamPos) != 0) break;
            Trace("StartEventPipeCounterSession: skipped %d bytes for Trace object", traceSize);
        }
        else
        {
            // All other blocks: int32 blockSize + alignment padding + blockSize bytes
            int32_t blockSize;
            if (read_exact_tracked(fd, &blockSize, 4, &streamPos) != 0) break;
            if (blockSize <= 0 || (size_t)blockSize > MAX_NETTRACE_BLOCK_SIZE)
            {
                Trace("StartEventPipeCounterSession: invalid blockSize=%d", blockSize);
                break;
            }

            // Compute alignment padding (align to 4 bytes)
            size_t curAlign = streamPos & 0x3;
            size_t alignPad = (4 - curAlign) & 0x3;
            if (alignPad > 0)
            {
                if (skip_bytes_tracked(fd, alignPad, &streamPos) != 0) break;
            }

            Trace("StartEventPipeCounterSession: block '%s' blockSize=%d alignPad=%zu", typeName, blockSize, alignPad);

            int blockRc = 0;
            if (strcmp(typeName, METADATA_BLOCK_TAG) == 0)
            {
                if (blockSize > 0) blockRc = parse_metadata_block(fd, blockSize, &state);
            }
            else if (strcmp(typeName, EVENT_BLOCK_TAG) == 0)
            {
                if (blockSize > 0) blockRc = parse_event_block(fd, blockSize, &state);
            }
            else
            {
                if (blockSize > 0) blockRc = skip_bytes(fd, blockSize);
            }
            if (blockRc != 0)
            {
                Trace("StartEventPipeCounterSession: block parse failed for '%s'", typeName);
                break;
            }
            // Advance stream position by blockSize (the parse/skip functions read exactly blockSize bytes)
            streamPos += blockSize;
        }

        // Read EndObject tag for the outer object
        uint8_t endTag;
        if (read_exact_tracked(fd, &endTag, 1, &streamPos) != 0) break;
        Trace("StartEventPipeCounterSession: endTag=%d", endTag);
    }

    set_session_fd(sessionFd, sessionFdMutex, -1);
    close(fd);
    Trace("StartEventPipeCounterSession: Exit");
    return 0;
}

//--------------------------------------------------------------------
//
// StopEventPipeSession
//
//--------------------------------------------------------------------
int StopEventPipeSession(const char* socketName, uint64_t sessionId)
{
    int fd = -1;
    struct sockaddr_un addr = {0};

    Trace("StopEventPipeSession: Enter (id=%lu)", (unsigned long)sessionId);

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
    {
        Trace("StopEventPipeSession: Failed to create socket [%d]", errno);
        return -1;
    }

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketName, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un)) == -1)
    {
        Trace("StopEventPipeSession: Failed to connect [%d]", errno);
        close(fd);
        return -1;
    }

    // Build StopTracing command (0x0201)
    size_t totalSize = sizeof(struct IpcHeader) + 8;
    uint8_t buffer[sizeof(struct IpcHeader) + 8];
    memset(buffer, 0, sizeof(buffer));

    struct IpcHeader* header = (struct IpcHeader*)buffer;
    memcpy(header->Magic, "DOTNET_IPC_V1", 14);
    header->Size = (uint16_t)totalSize;
    header->CommandSet = 0x02; // EventPipe
    header->CommandId = 0x01;  // StopTracing
    header->Reserved = 0;

    memcpy(buffer + sizeof(struct IpcHeader), &sessionId, 8);

    if (send_all(fd, buffer, totalSize) != 0)
    {
        Trace("StopEventPipeSession: Failed to send stop command [%d]", errno);
        close(fd);
        return -1;
    }

    // Read response
    struct IpcHeader responseHeader;
    if (recv_all(fd, &responseHeader, sizeof(responseHeader)) == 0)
    {
        // Read and discard payload
        if (responseHeader.Size > sizeof(responseHeader))
        {
            skip_bytes(fd, responseHeader.Size - sizeof(responseHeader));
        }
    }

    close(fd);
    Trace("StopEventPipeSession: Exit");
    return 0;
}
