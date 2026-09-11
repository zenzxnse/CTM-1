<script lang="ts">
  import { onMount } from 'svelte';
  import type { Health } from '$lib/api';

  type RecognitionResult = { 0: { transcript: string }; length: number };
  type RecognitionEvent = Event & {
    results: { [index: number]: RecognitionResult; length: number };
  };
  type Recognition = {
    lang: string;
    interimResults: boolean;
    continuous: boolean;
    onresult: ((event: RecognitionEvent) => void) | null;
    onerror: ((event: Event & { error?: string }) => void) | null;
    onend: (() => void) | null;
    start: () => void;
    stop: () => void;
  };
  type RecognitionConstructor = new () => Recognition;

  let {
    prompt = $bindable(),
    interpreter = $bindable(),
    busy,
    health,
    oncompose
  }: {
    prompt: string;
    interpreter: string;
    busy: boolean;
    health: Health | null;
    oncompose: () => void;
  } = $props();

  let examples = [
    'Show an overview of the assembly line',
    'Monitor the inspection station',
    'Show active alarms for the line'
  ];
  let promptLength = $derived(prompt.length);
  let speechSupported = $state<boolean | null>(null);
  let speechActive = $state(false);
  let speechStatus = $state('');
  let recognition: Recognition | null = null;
  let speechPrefix = '';

  onMount(() => {
    const candidate = window as unknown as {
      SpeechRecognition?: RecognitionConstructor;
      webkitSpeechRecognition?: RecognitionConstructor;
    };
    speechSupported = Boolean(candidate.SpeechRecognition ?? candidate.webkitSpeechRecognition);
  });

  function speechError(code: string | undefined): string {
    if (code === 'not-allowed' || code === 'service-not-allowed')
      return 'Microphone permission was denied. You can still type the request.';
    if (code === 'no-speech') return 'No speech was detected. Try again or type the request.';
    if (code === 'audio-capture') return 'The browser could not access an audio input.';
    return 'Browser dictation failed. The request field is still available for typing.';
  }

  function toggleSpeech() {
    if (speechActive) {
      recognition?.stop();
      return;
    }
    if (!speechSupported) {
      speechStatus = 'Browser dictation is unavailable here. Type the request instead.';
      return;
    }
    const candidate = window as unknown as {
      SpeechRecognition?: RecognitionConstructor;
      webkitSpeechRecognition?: RecognitionConstructor;
    };
    const Constructor = candidate.SpeechRecognition ?? candidate.webkitSpeechRecognition;
    if (!Constructor) {
      speechSupported = false;
      speechStatus = 'Browser dictation is unavailable here. Type the request instead.';
      return;
    }
    speechPrefix = prompt.trim();
    recognition = new Constructor();
    recognition.lang = 'en-AU';
    recognition.interimResults = true;
    recognition.continuous = false;
    recognition.onresult = (event) => {
      const transcript = Array.from(
        { length: event.results.length },
        (_, index) => event.results[index]?.[0]?.transcript ?? ''
      )
        .join(' ')
        .trim();
      if (transcript)
        prompt = `${speechPrefix}${speechPrefix ? ' ' : ''}${transcript}`.slice(0, 2048);
    };
    recognition.onerror = (event) => {
      speechStatus = speechError(event.error);
      speechActive = false;
    };
    recognition.onend = () => {
      speechActive = false;
      if (!speechStatus) speechStatus = 'Transcript added. Review it before submitting.';
    };
    speechStatus = 'Listening in the browser…';
    speechActive = true;
    try {
      recognition.start();
    } catch {
      speechActive = false;
      speechStatus = 'Browser dictation could not start. Type the request instead.';
    }
  }
</script>

<section class="request-panel" aria-labelledby="request-title">
  <h2 id="request-title">What do you need to see?</h2>
  <p>
    Describe a supported task and the equipment involved. The service validates the request and
    resolves every binding from the declared model. Screens are assembled from current model
    components, not predesigned pages.
  </p>
  <form
    onsubmit={(event) => {
      event.preventDefault();
      oncompose();
    }}
  >
    <label for="request" class="sr-only">Operator request</label>
    <textarea
      id="request"
      bind:value={prompt}
      maxlength="2048"
      rows="2"
      placeholder="Ask for a view of declared equipment, readings, or alarms"
      disabled={busy}></textarea>
    <div class="speech-row">
      <button
        type="button"
        class="small-button speech-button"
        class:speech-active={speechActive}
        disabled={busy || speechSupported === null}
        aria-pressed={speechActive}
        onclick={toggleSpeech}
        >{speechActive ? 'Stop dictation' : 'Use browser dictation'}
        <span aria-hidden="true">◉</span></button
      >
      <span class="speech-status" role="status">
        {#if speechStatus}{speechStatus}{:else if speechSupported === false}Browser dictation
          unavailable{:else}Audio is handled by the browser; only resulting text is submitted.{/if}
      </span>
    </div>
    <p class="char-count" aria-hidden="true">{promptLength} / 2048 characters</p>
    <div class="request-actions">
      <div class="interpreter-select">
        <label for="interpreter">Interpret with</label><select
          id="interpreter"
          bind:value={interpreter}
          disabled={busy}
        >
          <option value="rules">Rules · offline</option>
          <option value="llama" disabled={!health?.llama_configured}
            >Local AI · llama.cpp{health?.llama_configured ? '' : ' (not configured)'}</option
          >
        </select>
      </div>
      <button class="primary-button" type="submit" disabled={busy || !health || !prompt.trim()}
        >{busy ? 'Resolving…' : 'Compose view'}<span aria-hidden="true">↗</span></button
      >
    </div>
  </form>
  <div class="examples">
    <span>Try</span>
    {#each examples as example (example)}
      <button
        type="button"
        disabled={busy}
        onclick={() => {
          prompt = example;
        }}>{example}</button
      >
    {/each}
  </div>
</section>
