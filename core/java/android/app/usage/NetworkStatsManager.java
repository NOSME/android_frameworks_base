/**
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

package android.app.usage;

import static com.android.internal.util.Preconditions.checkNotNull;

import android.annotation.Nullable;
import android.app.usage.NetworkStats.Bucket;
import android.content.Context;
import android.net.ConnectivityManager;
import android.net.DataUsageRequest;
import android.net.NetworkIdentity;
import android.net.NetworkTemplate;
import android.net.INetworkStatsService;
import android.os.Binder;
import android.os.Build;
import android.os.Message;
import android.os.Messenger;
import android.os.Handler;
import android.os.Looper;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.util.Log;

/**
 * Provides access to network usage history and statistics. Usage data is collected in
 * discrete bins of time called 'Buckets'. See {@link NetworkStats.Bucket} for details.
 * <p />
 * Queries can define a time interval in the form of start and end timestamps (Long.MIN_VALUE and
 * Long.MAX_VALUE can be used to simulate open ended intervals). By default, apps can only obtain
 * data about themselves. See the below note for special cases in which apps can obtain data about
 * other applications.
 * <h3>
 * Summary queries
 * </h3>
 * {@link #querySummaryForDevice} <p />
 * {@link #querySummaryForUser} <p />
 * {@link #querySummary} <p />
 * These queries aggregate network usage across the whole interval. Therefore there will be only one
 * bucket for a particular key and state and roaming combination. In case of the user-wide and
 * device-wide summaries a single bucket containing the totalised network usage is returned.
 * <h3>
 * History queries
 * </h3>
 * {@link #queryDetailsForUid} <p />
 * {@link #queryDetails} <p />
 * These queries do not aggregate over time but do aggregate over state and roaming. Therefore there
 * can be multiple buckets for a particular key but all Bucket's state is going to be
 * {@link NetworkStats.Bucket#STATE_ALL} and all Bucket's roaming is going to be
 * {@link NetworkStats.Bucket#ROAMING_ALL}.
 * <p />
 * <b>NOTE:</b> Calling {@link #querySummaryForDevice} or accessing stats for apps other than the
 * calling app requires the permission {@link android.Manifest.permission#PACKAGE_USAGE_STATS},
 * which is a system-level permission and will not be granted to third-party apps. However,
 * declaring the permission implies intention to use the API and the user of the device can grant
 * permission through the Settings application.
 * <p />
 * Profile owner apps are automatically granted permission to query data on the profile they manage
 * (that is, for any query except {@link #querySummaryForDevice}). Device owner apps and carrier-
 * privileged apps likewise get access to usage data for all users on the device.
 * <p />
 * In addition to tethering usage, usage by removed users and apps, and usage by the system
 * is also included in the results for callers with one of these higher levels of access.
 * <p />
 * <b>NOTE:</b> Prior to API level {@value Build.VERSION_CODES#N}, all calls to these APIs required
 * the above permission, even to access an app's own data usage, and carrier-privileged apps were
 * not included.
 */
public class NetworkStatsManager {
    private static final String TAG = "NetworkStatsManager";
    private static final boolean DBG = false;

    /** @hide */
    public static final int CALLBACK_LIMIT_REACHED = 0;
    /** @hide */
    public static final int CALLBACK_RELEASED = 1;

    private final Context mContext;
    private final INetworkStatsService mService;

    /**
     * {@hide}
     */
    public NetworkStatsManager(Context context) {
        mContext = context;
        mService = INetworkStatsService.Stub.asInterface(
                ServiceManager.getService(Context.NETWORK_STATS_SERVICE));
    }

    /**
     * Query network usage statistics summaries. Result is summarised data usage for the whole
     * device. Result is a single Bucket aggregated over time, state, uid, tag and roaming. This
     * means the bucket's start and end timestamp are going to be the same as the 'startTime' and
     * 'endTime' parameters. State is going to be {@link NetworkStats.Bucket#STATE_ALL}, uid
     * {@link NetworkStats.Bucket#UID_ALL}, tag {@link NetworkStats.Bucket#TAG_ALL}
     * and roaming {@link NetworkStats.Bucket#ROAMING_ALL}.
     *
     * @param networkType As defined in {@link ConnectivityManager}, e.g.
     *            {@link ConnectivityManager#TYPE_MOBILE}, {@link ConnectivityManager#TYPE_WIFI}
     *            etc.
     * @param subscriberId If applicable, the subscriber id of the network interface.
     * @param startTime Start of period. Defined in terms of "Unix time", see
     *            {@link java.lang.System#currentTimeMillis}.
     * @param endTime End of period. Defined in terms of "Unix time", see
     *            {@link java.lang.System#currentTimeMillis}.
     * @return Bucket object or null if permissions are insufficient or error happened during
     *         statistics collection.
     */
    public Bucket querySummaryForDevice(int networkType, String subscriberId,
            long startTime, long endTime) throws SecurityException, RemoteException {
        NetworkTemplate template = createTemplate(networkType, subscriberId);
        if (template == null) {
            return null;
        }

        Bucket bucket = null;
        NetworkStats stats = new NetworkStats(mContext, template, startTime, endTime);
        bucket = stats.getDeviceSummaryForNetwork();

        stats.close();
        return bucket;
    }

    /**
     * Query network usage statistics summaries aggregated across tags.
     *
     * #see querySummaryForUser(int, String, long, long, boolean)
     */
    public Bucket querySummaryForUser(int networkType, String subscriberId, long startTime,
            long endTime) throws SecurityException, RemoteException {
        return querySummaryForUser(networkType, subscriberId, startTime, endTime,
            false /* includeTags */);
    }

    /**
     * Query network usage statistics summaries. Result is summarised data usage for all uids
     * belonging to calling user. Result is a single Bucket aggregated over time, state and uid.
     * This means the bucket's start and end timestamp are going to be the same as the 'startTime'
     * and 'endTime' parameters. State is going to be {@link NetworkStats.Bucket#STATE_ALL} and uid
     * {@link NetworkStats.Bucket#UID_ALL}.
     *
     * @param networkType As defined in {@link ConnectivityManager}, e.g.
     *            {@link ConnectivityManager#TYPE_MOBILE}, {@link ConnectivityManager#TYPE_WIFI}
     *            etc.
     * @param subscriberId If applicable, the subscriber id of the network interface.
     * @param startTime Start of period. Defined in terms of "Unix time", see
     *            {@link java.lang.System#currentTimeMillis}.
     * @param endTime End of period. Defined in terms of "Unix time", see
     *            {@link java.lang.System#currentTimeMillis}.
     * @param includeTags whether to include network tags. If {@code true}, tags will be returned
     *            and history retention may be shorter.
     * @return Bucket object or null if permissions are insufficient or error happened during
     *         statistics collection.
     */
    public Bucket querySummaryForUser(int networkType, String subscriberId, long startTime,
            long endTime, boolean includeTags) throws SecurityException, RemoteException {
        NetworkTemplate template = createTemplate(networkType, subscriberId);
        if (template == null) {
            return null;
        }

        NetworkStats stats;
        stats = new NetworkStats(mContext, template, startTime, endTime);
        stats.startSummaryEnumeration(includeTags);

        stats.close();
        return stats.getSummaryAggregate();
    }

    /**
     * Query network usage statistics summaries aggregated across tags.
     *
     * #see querySummary(int, String, long, long, boolean)
     */
    public NetworkStats querySummary(int networkType, String subscriberId, long startTime,
            long endTime) throws SecurityException, RemoteException {
        return querySummary(networkType, subscriberId, startTime, endTime, false /* includeTags */);
    }

    /**
     * Query network usage statistics summaries. Result filtered to include only uids belonging to
     * calling user. Result is aggregated over time, hence all buckets will have the same start and
     * end timestamps. Not aggregated over state or uid or tag. This means buckets' start and end
     * timestamps are going to be the same as the 'startTime' and 'endTime' parameters. State,
     * uid and tag are going to vary.
     *
     * @param networkType As defined in {@link ConnectivityManager}, e.g.
     *            {@link ConnectivityManager#TYPE_MOBILE}, {@link ConnectivityManager#TYPE_WIFI}
     *            etc.
     * @param subscriberId If applicable, the subscriber id of the network interface.
     * @param startTime Start of period. Defined in terms of "Unix time", see
     *            {@link java.lang.System#currentTimeMillis}.
     * @param endTime End of period. Defined in terms of "Unix time", see
     *            {@link java.lang.System#currentTimeMillis}.
     * @param includeTags whether to include network tags. If {@code true}, tags will be returned
     *            and history retention may be shorter.
     * @return Statistics object or null if permissions are insufficient or error happened during
     *         statistics collection.
     */
    public NetworkStats querySummary(int networkType, String subscriberId, long startTime,
            long endTime, boolean includeTags) throws SecurityException, RemoteException {
        NetworkTemplate template = createTemplate(networkType, subscriberId);
        if (template == null) {
            return null;
        }

        NetworkStats result;
        result = new NetworkStats(mContext, template, startTime, endTime);
        result.startSummaryEnumeration(includeTags);

        return result;
    }

    /**
     * Query network usage statistics details for a given uid.
     *
     * #see queryDetailsForUidTag(int, String, long, long, int, int)
     */
    public NetworkStats queryDetailsForUid(int networkType, String subscriberId,
            long startTime, long endTime, int uid) throws SecurityException, RemoteException {
        return queryDetailsForUidTag(networkType, subscriberId, startTime, endTime, uid,
            NetworkStats.Bucket.TAG_ALL);
    }

    /**
     * Query network usage statistics details for a given uid and tag. Only usable for uids
     * belonging to calling user. Result is aggregated over state but not aggregated over time.
     * This means buckets' start and end timestamps are going to be between 'startTime' and
     * 'endTime' parameters. State is going to be {@link NetworkStats.Bucket#STATE_ALL}, uid the
     * same as the 'uid' parameter and tag the same as 'tag' parameter.
     * <p>Only includes buckets that atomically occur in the inclusive time range. Doesn't
     * interpolate across partial buckets. Since bucket length is in the order of hours, this
     * method cannot be used to measure data usage on a fine grained time scale.
     *
     * @param networkType As defined in {@link ConnectivityManager}, e.g.
     *            {@link ConnectivityManager#TYPE_MOBILE}, {@link ConnectivityManager#TYPE_WIFI}
     *            etc.
     * @param subscriberId If applicable, the subscriber id of the network interface.
     * @param startTime Start of period. Defined in terms of "Unix time", see
     *            {@link java.lang.System#currentTimeMillis}.
     * @param endTime End of period. Defined in terms of "Unix time", see
     *            {@link java.lang.System#currentTimeMillis}.
     * @param uid UID of app
     * @param tag TAG of interest. Use {@link NetworkStats.Bucket#TAG_ANY} for any tags, use
     *            {@link NetworkStats.Bucket#TAG_ALL} to aggregate over tags.
     * @return Statistics object or null if permissions are insufficient or error happened during
     *         statistics collection.
     */
    public NetworkStats queryDetailsForUidTag(int networkType, String subscriberId,
            long startTime, long endTime, int uid, int tag) throws SecurityException,
            RemoteException {
        NetworkTemplate template = createTemplate(networkType, subscriberId);
        if (template == null) {
            return null;
        }

        NetworkStats result;
        result = new NetworkStats(mContext, template, startTime, endTime);
        result.startHistoryEnumeration(uid, tag);

        return result;
    }

    /**
     * Query network usage statistics details. Result filtered to include only uids belonging to
     * calling user. Result is aggregated over state but not aggregated over time or uid. This means
     * buckets' start and end timestamps are going to be between 'startTime' and 'endTime'
     * parameters. State is going to be {@link NetworkStats.Bucket#STATE_ALL}, uid will vary,
     * tag {@link NetworkStats.Bucket#TAG_ALL} and roaming is going to be
     * {@link NetworkStats.Bucket#ROAMING_ALL}.
     * <p>Only includes buckets that atomically occur in the inclusive time range. Doesn't
     * interpolate across partial buckets. Since bucket length is in the order of hours, this
     * method cannot be used to measure data usage on a fine grained time scale.
     *
     * @param networkType As defined in {@link ConnectivityManager}, e.g.
     *            {@link ConnectivityManager#TYPE_MOBILE}, {@link ConnectivityManager#TYPE_WIFI}
     *            etc.
     * @param subscriberId If applicable, the subscriber id of the network interface.
     * @param startTime Start of period. Defined in terms of "Unix time", see
     *            {@link java.lang.System#currentTimeMillis}.
     * @param endTime End of period. Defined in terms of "Unix time", see
     *            {@link java.lang.System#currentTimeMillis}.
     * @return Statistics object or null if permissions are insufficient or error happened during
     *         statistics collection.
     */
    public NetworkStats queryDetails(int networkType, String subscriberId, long startTime,
            long endTime) throws SecurityException, RemoteException {
        NetworkTemplate template = createTemplate(networkType, subscriberId);
        if (template == null) {
            return null;
        }
        NetworkStats result;
        result = new NetworkStats(mContext, template, startTime, endTime);
        result.startUserUidEnumeration();
        return result;
    }

    /**
     * Registers to receive notifications about data usage on specified networks and uids.
     * The callbacks will continue to be called as long as the process is live or
     * {@link #unregisterDataUsageCallback} is called.
     *
     * @param policy {@link DataUsagePolicy} describing this request.
     * @param callback The {@link DataUsageCallback} that the system will call when data usage
     *            has exceeded the specified threshold.
     */
    public void registerDataUsageCallback(DataUsagePolicy policy, DataUsageCallback callback) {
        registerDataUsageCallback(policy, callback, null /* handler */);
    }

    /**
     * Registers to receive notifications about data usage on specified networks and uids.
     * The callbacks will continue to be called as long as the process is live or
     * {@link #unregisterDataUsageCallback} is called.
     *
     * @param policy {@link DataUsagePolicy} describing this request.
     * @param callback The {@link DataUsageCallback} that the system will call when data usage
     *            has exceeded the specified threshold.
     * @param handler to dispatch callback events through, otherwise if {@code null} it uses
     *            the calling thread.
     */
    public void registerDataUsageCallback(DataUsagePolicy policy, DataUsageCallback callback,
                @Nullable Handler handler) {
        checkNotNull(policy, "DataUsagePolicy cannot be null");
        checkNotNull(callback, "DataUsageCallback cannot be null");

        final Looper looper;
        if (handler == null) {
            looper = Looper.myLooper();
        } else {
            looper = handler.getLooper();
        }

        if (DBG) Log.d(TAG, "registerDataUsageCallback called with " + policy);

        NetworkTemplate[] templates;
        if (policy.subscriberIds == null || policy.subscriberIds.length == 0) {
            templates = new NetworkTemplate[1];
            templates[0] = createTemplate(policy.networkType, null /* subscriberId */);
        } else {
            templates = new NetworkTemplate[policy.subscriberIds.length];
            for (int i = 0; i < policy.subscriberIds.length; i++) {
                templates[i] = createTemplate(policy.networkType, policy.subscriberIds[i]);
            }
        }
        DataUsageRequest request = new DataUsageRequest(DataUsageRequest.REQUEST_ID_UNSET,
                templates, policy.uids, policy.thresholdInBytes);
        try {
            CallbackHandler callbackHandler = new CallbackHandler(looper, callback);
            callback.request = mService.registerDataUsageCallback(
                    mContext.getOpPackageName(), request, new Messenger(callbackHandler),
                    new Binder());
            if (DBG) Log.d(TAG, "registerDataUsageCallback returned " + callback.request);

            if (callback.request == null) {
                Log.e(TAG, "Request from callback is null; should not happen");
            }
        } catch (RemoteException e) {
            if (DBG) Log.d(TAG, "Remote exception when registering callback");
        }
    }

    /**
     * Unregisters callbacks on data usage.
     *
     * @param callback The {@link DataUsageCallback} used when registering.
     */
    public void unregisterDataUsageCallback(DataUsageCallback callback) {
        if (callback == null || callback.request == null
                || callback.request.requestId == DataUsageRequest.REQUEST_ID_UNSET) {
            throw new IllegalArgumentException("Invalid DataUsageCallback");
        }
        try {
            mService.unregisterDataUsageRequest(callback.request);
        } catch (RemoteException e) {
            if (DBG) Log.d(TAG, "Remote exception when unregistering callback");
        }
    }

    /**
     * Base class for data usage callbacks. Should be extended by applications wanting
     * notifications.
     */
    public static class DataUsageCallback {
        /**
         * Called when data usage has reached the given policy threshold.
         */
        public void onLimitReached() {}

        private DataUsageRequest request;
    }

    private static NetworkTemplate createTemplate(int networkType, String subscriberId) {
        NetworkTemplate template = null;
        switch (networkType) {
            case ConnectivityManager.TYPE_MOBILE: {
                template = NetworkTemplate.buildTemplateMobileAll(subscriberId);
                } break;
            case ConnectivityManager.TYPE_WIFI: {
                template = NetworkTemplate.buildTemplateWifiWildcard();
                } break;
            default: {
                Log.w(TAG, "Cannot create template for network type " + networkType
                        + ", subscriberId '" + NetworkIdentity.scrubSubscriberId(subscriberId) +
                        "'.");
            }
        }
        return template;
    }

    private static class CallbackHandler extends Handler {
        private DataUsageCallback mCallback;
        CallbackHandler(Looper looper, DataUsageCallback callback) {
            super(looper);
            mCallback = callback;
        }

        @Override
        public void handleMessage(Message message) {
            DataUsageRequest request =
                    (DataUsageRequest) getObject(message, DataUsageRequest.PARCELABLE_KEY);

            switch (message.what) {
                case CALLBACK_LIMIT_REACHED: {
                    if (mCallback != null) {
                        mCallback.onLimitReached();
                    } else {
                        Log.e(TAG, "limit reached with released callback for " + request);
                    }
                    break;
                }
                case CALLBACK_RELEASED: {
                    if (DBG) Log.d(TAG, "callback released for " + request);
                    mCallback = null;
                    break;
                }
            }
        }

        private static Object getObject(Message msg, String key) {
            return msg.getData().getParcelable(key);
        }
    }
}
