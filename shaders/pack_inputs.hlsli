// Typed scene inputs and audio inputs for shader packs (docs/shader_effect_api.md, docs/shader_pt_api.md).
// Included by pack_api.hlsli, pt_pack_api.hlsli, and effect_api.hlsli.
// Functions are compiled in when the pack declares the respective capability
// (PACK_CAP_INPUTS_SCENE / PACK_CAP_INPUTS_AUDIO), so packs that do not require them compile
// byte-identically to before.
#ifndef PACK_INPUTS_HLSLI
#define PACK_INPUTS_HLSLI

// Role IDs matching PackInput order (src/render/ShaderPack.h)
#define PACK_IN_PERFORMER_HEAD   0u
#define PACK_IN_PERFORMER_CENTER 1u
#define PACK_IN_PERFORMER_HAND_L 2u
#define PACK_IN_PERFORMER_HAND_R 3u
#define PACK_IN_PERFORMER_FOOT_L 4u
#define PACK_IN_PERFORMER_FOOT_R 5u
#define PACK_IN_LIGHT_KEY        6u
#define PACK_IN_CAMERA_FOCUS     7u
#define PACK_IN_AUDIO            8u

#endif // PACK_INPUTS_HLSLI

#if (defined(PACK_CAP_INPUTS_SCENE) && PACK_CAP_INPUTS_SCENE) && !defined(PACK_INPUTS_SCENE_DECLARED)
#define PACK_INPUTS_SCENE_DECLARED

bool PackInputValid(uint role) {
    if (role == PACK_IN_AUDIO) {
#if (defined(PACK_CAP_INPUTS_AUDIO) && PACK_CAP_INPUTS_AUDIO)
        return gPackAudio1.z > 0.5;
#else
        return false;
#endif
    }
    if (role >= 8u) return false;
    return gPackRoles[role].posValid.w > 0.5;
}

float3 PackInputPos(uint role) {
    if (role >= 8u) return float3(0.0, 0.0, 0.0);
    return gPackRoles[role].posValid.xyz;
}

float3x3 PackInputFrame(uint role) {
    if (role >= 8u) return float3x3(1.0, 0.0, 0.0,  0.0, 1.0, 0.0,  0.0, 0.0, 1.0);
    return float3x3(gPackRoles[role].right.xyz,
                    gPackRoles[role].up.xyz,
                    gPackRoles[role].forward.xyz);
}

#endif // PACK_CAP_INPUTS_SCENE

#if (defined(PACK_CAP_INPUTS_AUDIO) && PACK_CAP_INPUTS_AUDIO) && !defined(PACK_INPUTS_AUDIO_DECLARED)
#define PACK_INPUTS_AUDIO_DECLARED

#if !defined(PACK_INPUTS_SCENE_DECLARED)
bool PackInputValid(uint role) {
    if (role == PACK_IN_AUDIO) return gPackAudio1.z > 0.5;
    return false;
}
#endif

float PackAudioBeat() {
    return gPackAudio0.x;
}

float PackAudioDownbeat() {
    return gPackAudio0.y;
}

float PackAudioOnset() {
    return gPackAudio0.z;
}

float PackAudioEnergy() {
    return gPackAudio0.w;
}

float PackAudioSection() {
    return gPackAudio1.x;
}

float PackAudioBpm() {
    return gPackAudio1.y;
}

// Continuous beat count: index of the current beat + its phase (negative before the first beat).
float PackAudioBeatCount() {
    return gPackAudio1.w;
}

#endif // PACK_CAP_INPUTS_AUDIO
