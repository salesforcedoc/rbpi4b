/* SPDX-License-Identifier: MIT */
/* Optional knobshim callback, compiled inside the adapted Samples module.
 * Use the same state as the native panel so MIDI and touch share one level. */
__attribute__((visibility("default")))
int rx3_pi_samples_volume(unsigned int volume)
{
    if (!__atomic_load_n(&samples_callbacks_enabled, __ATOMIC_SEQ_CST))
        return 0;
    if (volume > SAMPLES_VOLUME_MAX)
        volume = SAMPLES_VOLUME_MAX;
    __atomic_store_n(&samples_volume_touched, 1u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&samples_volume, volume, __ATOMIC_SEQ_CST);
    return 1;
}
