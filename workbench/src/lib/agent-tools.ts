type ToolRegistration = {
  name: string;
  title: string;
  description: string;
  inputSchema: object;
  annotations: { readOnlyHint: boolean; untrustedContentHint: boolean };
  execute: (input: unknown) => unknown | Promise<unknown>;
};
type ModelContext = {
  registerTool: (tool: ToolRegistration, options: { signal: AbortSignal }) => void | Promise<void>;
};

/** The optional browser registry uses the same visible request flow and runtime validation. */
export function registerWorkbenchTools(
  compose: (prompt: string) => Promise<unknown>,
  read: () => unknown
): () => void {
  const context = (document as Document & { modelContext?: ModelContext }).modelContext;
  if (!context?.registerTool) return () => {};
  const lifecycle = new AbortController();
  const tools: ToolRegistration[] = [
    {
      name: 'read_hmi_task_view',
      title: 'Read current HMI task view',
      description: 'Read the current task, model revision and unresolved binding issues.',
      inputSchema: { type: 'object', properties: {}, additionalProperties: false },
      annotations: { readOnlyHint: true, untrustedContentHint: true },
      execute(input) {
        if (
          input === null ||
          typeof input !== 'object' ||
          Array.isArray(input) ||
          Object.keys(input).length
        )
          throw new Error('This operation takes an empty object.');
        return read();
      }
    },
    {
      name: 'compose_hmi_task_view',
      title: 'Compose an HMI task view',
      description:
        'Submit an operator request and update the visible view using the selected interpreter. This does not control equipment.',
      inputSchema: {
        type: 'object',
        properties: { prompt: { type: 'string', minLength: 1, maxLength: 2048 } },
        required: ['prompt'],
        additionalProperties: false
      },
      annotations: { readOnlyHint: false, untrustedContentHint: true },
      async execute(input) {
        if (
          input === null ||
          typeof input !== 'object' ||
          Array.isArray(input) ||
          Object.keys(input).length !== 1 ||
          !('prompt' in input) ||
          typeof input.prompt !== 'string' ||
          !input.prompt.trim() ||
          input.prompt.length > 2048
        )
          throw new Error('Provide one prompt of 1 to 2048 characters.');
        return await compose(input.prompt);
      }
    }
  ];
  for (const tool of tools) {
    try {
      void Promise.resolve(context.registerTool(tool, { signal: lifecycle.signal })).catch(() =>
        lifecycle.abort()
      );
    } catch {
      lifecycle.abort();
    }
  }
  return () => lifecycle.abort();
}
