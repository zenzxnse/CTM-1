<script lang="ts">
  let {
    role = $bindable()
  }: {
    role: 'operator' | 'engineer' | 'supervisor';
  } = $props();

  let roles: { id: 'operator' | 'engineer' | 'supervisor'; label: string; hint: string }[] = [
    { id: 'operator', label: 'Operator', hint: 'Alarms first' },
    { id: 'engineer', label: 'Engineer', hint: 'Bindings open' },
    { id: 'supervisor', label: 'Supervisor', hint: 'Summary first' }
  ];
</script>

<section class="role-filter" aria-labelledby="role-title">
  <p class="sidebar-heading" id="role-title">Role emphasis</p>
  <fieldset>
    <legend class="sr-only">Role emphasis</legend>
    {#each roles as item (item.id)}
      <label class="role-option" class:active={role === item.id}>
        <input type="radio" name="role" value={item.id} bind:group={role} />{item.label}<small
          >{item.hint}</small
        >
      </label>
    {/each}
  </fieldset>
  <p class="role-note">A display preference only. It never grants capability.</p>
</section>
