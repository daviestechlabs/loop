#ifndef VOICE_C_SUBJECTS_H
#define VOICE_C_SUBJECTS_H

/* NATS subjects for pure-C voice services (mirror fleet contracts). */

#define SUBJ_VOICE_STREAM_ALL          "ai.voice.stream.>"
#define SUBJ_VOICE_PCM_ALL             "ai.voice.pcm.>"
#define SUBJ_VOICE_PCM_PFX             "ai.voice.pcm"
#define SUBJ_VOICE_TRANSCRIPTION_PFX   "ai.voice.transcription"
#define SUBJ_VOICE_REFLEX_PFX          "ai.voice.reflex"
#define SUBJ_VOICE_UTTERANCE_PFX       "ai.voice.utterance"
#define SUBJ_TURN_START                "ai.turn.start"
#define SUBJ_TURN_GENERATE             "ai.turn.generate"
#define SUBJ_TURN_TOKEN                "ai.turn.token"
#define SUBJ_TURN_CANCEL               "ai.turn.cancel"
#define SUBJ_VOICE_STREAM_PFX          "ai.voice.stream"
#define SUBJ_TURN_EVENTS_PFX           "ai.turn.events"
#define SUBJ_TURN_TTS_SPEAK            "ai.turn.tts.speak"
#define SUBJ_TURN_TTS_PCM              "ai.turn.tts.pcm"
#define SUBJ_SESSION_APPEND            "ai.session.append"
#define SUBJ_SESSION_GET               "ai.session.get"
#define SUBJ_SESSION_DELETE            "ai.session.delete"
#define SUBJ_SESSION_SUMMARY           "ai.session.summary.get"
#define SUBJ_RAG_SEARCH                "ai.rag.search"
#define SUBJ_VOICE_TURN_PREPARE        "ai.voice.turn.prepare"
#define SUBJ_VOICES_LIST               "ai.tts.voices.list"

#endif
