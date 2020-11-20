/**
 * Copyright (c) 2020, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.media.tv.tuner;

import android.media.tv.tuner.TunerServiceFrontendInfo;

/**
 * TunerService interface handles tuner related operations.
 *
 * {@hide}
 */
interface ITunerService {

    /**
     * Gets frontend IDs.
     *
     * @return the result code of the operation.
     */
    int getFrontendIds(out int[] ids);

    /**
     * Retrieve the frontend's information.
     *
     * @param frontendHandle the handle of the frontend granted by TRM.
     * @return the information for the frontend.
     */
    TunerServiceFrontendInfo getFrontendInfo(in int frontendHandle);
}