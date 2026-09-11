<script lang="ts">
  import type { ExecutionInfo } from '$lib/api';

  let { execution }: { execution: ExecutionInfo | null } = $props();

  function number(value: number | undefined, suffix = ''): string {
    return value === undefined || !Number.isFinite(value) ? 'Not reported' : `${value}${suffix}`;
  }

  function cacheDetail(
    cache: { state?: string; hits?: number; misses?: number } | undefined
  ): string {
    if (!cache) return 'Not reported';
    const state = cache.state ?? 'unknown';
    const counts = [
      cache.hits !== undefined ? `${cache.hits} hits` : '',
      cache.misses !== undefined ? `${cache.misses} misses` : ''
    ]
      .filter(Boolean)
      .join(' · ');
    return counts ? `${state} · ${counts}` : state;
  }
</script>

<section class="execution-panel" aria-labelledby="execution-title">
  <div class="section-heading">
    <div>
      <span class="eyebrow">Explainability</span>
      <h2 id="execution-title">Request execution</h2>
    </div>
    {#if execution?.elapsed_ms !== undefined}<span class="timing-value"
        >{number(execution.elapsed_ms, ' ms')}</span
      >{/if}
  </div>
  {#if execution === null}
    <p class="section-note">Execution details appear after the service interprets a request.</p>
  {:else}
    <div class="execution-grid">
      <div class="execution-stat">
        <span>Interpretation cache</span>
        <strong>{cacheDetail(execution.cache?.interpretation)}</strong>
      </div>
      <div class="execution-stat">
        <span>Retrieval cache</span>
        <strong>{cacheDetail(execution.cache?.retrieval)}</strong>
      </div>
      <div class="execution-stat">
        <span>Context</span>
        <strong>{number(execution.context?.bytes, ' bytes')}</strong>
        <small>{number(execution.context?.estimated_tokens)} estimated tokens</small>
      </div>
      <div class="execution-stat">
        <span>Provider tokens</span>
        <strong>{number(execution.provider_usage?.total_tokens)}</strong>
        <small
          >{execution.provider_usage?.reported === false
            ? 'Provider did not report usage'
            : `${number(execution.provider_usage?.cached_prompt_tokens)} cached prompt`}</small
        >
      </div>
    </div>
    <dl class="execution-facts">
      <div>
        <dt>Model</dt>
        <dd><code>{execution.model_id ?? 'Unknown'}</code></dd>
      </div>
      <div>
        <dt>Revision</dt>
        <dd>{execution.model_revision ?? 'Unknown'}</dd>
      </div>
      <div>
        <dt>Generation</dt>
        <dd>{execution.context_generation ?? 'Unknown'}</dd>
      </div>
      <div>
        <dt>Candidates</dt>
        <dd>{execution.context?.candidates ?? 'Unknown'}</dd>
      </div>
      <div>
        <dt>Assets / tags</dt>
        <dd>{execution.context?.assets ?? '?'} / {execution.context?.tags ?? '?'}</dd>
      </div>
      <div>
        <dt>Relationships / alarms</dt>
        <dd>{execution.context?.relationships ?? '?'} / {execution.context?.alarms ?? '?'}</dd>
      </div>
      <div>
        <dt>Truncated</dt>
        <dd>
          {execution.context?.truncated === undefined
            ? 'Unknown'
            : execution.context.truncated
              ? 'Yes, bounded'
              : 'No'}
        </dd>
      </div>
      <div>
        <dt>Completion tokens</dt>
        <dd>{number(execution.provider_usage?.completion_tokens)}</dd>
      </div>
    </dl>
    {#if execution.errors?.length}
      <div class="view-issues" role="status">
        <strong>Execution warnings</strong>
        <ul>
          {#each execution.errors as issue (issue.code + issue.message)}<li>
              {issue.message}
            </li>{/each}
        </ul>
      </div>
    {/if}
  {/if}
</section>
