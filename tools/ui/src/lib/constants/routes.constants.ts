/** Query params the chat routes read from the URL. */
export const URL_PARAMS = {
	/** Load the selected model instead of waiting for the first message. */
	LOAD: 'load',
	/** Model to select. */
	MODEL: 'model',
	/** Prompt to send on arrival. */
	QUERY: 'q'
} as const;

export const ROUTES = {
	AUTOPILOT: '#/caliber-advisor',
	CALIBER_ADVISOR: '#/caliber-advisor',
	/** Chat base - for dynamic chat URLs use RouterService. */
	CHAT: '#/chat',
	DS4_BENCH: '#/ds4-bench',
	/** Local model evaluation and advisor dashboards. */
	DS4_EVAL: '#/ds4-eval',
	FIT_ADVISOR: '#/fit-advisor',
	/** MCP servers. */
	MCP_SERVERS: '#/mcp-servers',
	/** Search - mobile-only full-page conversation search. */
	SEARCH: '#/search',
	/** Settings base - for dynamic settings URLs use RouterService. */
	SETTINGS: '#/settings',
	/** Root - start of the app. */
	START: '#/'
} as const;
