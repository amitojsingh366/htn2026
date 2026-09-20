import { Agent, type Connection } from 'agents';
import OpenAI from 'openai';
import { toResponseInputItems } from 'openai/lib/responses/ResponseInputItems';
import type { ResponseInput, ResponseFunctionToolCall } from 'openai/resources/responses/responses';
import type { DirectorAction, DirectorObservation, DirectorRun, DirectorState, DirectorStatus, JsonValue } from './director-types';
import { cleanAnnouncement, groundedRecap, initialDirectorState, limits, MAX_HISTORY, MAX_REQUESTS_PER_RUN, meaningfulChange, observationKey } from './director-policy';
import { DIRECTOR_INSTRUCTIONS, DIRECTOR_TOOLS } from './director-tools';

type Wake = { roundId: string; dueAt: number };
type FollowUp = { id: string; roundId: string; reason: string };
const MAX_RUN_MS = 90_000;
class StaleRun extends Error {}

/** Cloudflare owns durable memory, scheduling and tools. OpenAI supplies decisions. */
export class OutbreakDirector extends Agent<Env, DirectorState> {
  initialState = initialDirectorState();

  override validateStateChange(_state: DirectorState, source: Connection | 'server'): void {
    if (source !== 'server') throw new Error('Director state is server-owned.');
  }
  override onRequest(): Response { return new Response('Not found', { status: 404 }); }
  override onConnect(connection: Connection): void { connection.close(1008, 'No public agent connections'); }

  override async onStart(): Promise<void> {
    // An interrupted model request is never automatically retried. Its reserved
    // budget survives restarts, as do every tool intent and gateway action ID.
    if (this.state.activeRun) {
      const id = this.state.activeRun.id;
      this.setState({ ...this.state, activeRun: null, status: 'degraded', reason: 'Previous run was interrupted; waiting for a new observation.',
        runs: this.state.runs.map(run => run.id === id ? { ...run, status: 'failed', summary: 'Interrupted; no automatic API retry.' } : run) });
    }
    if (this.state.pending) await this.scheduleWake();
  }

  private enabled(): boolean { return this.env.DIRECTOR_ENABLED === 'true'; }
  private configured(): boolean { return Boolean(this.env.OPENAI_API_KEY?.trim()); }
  private model(): string { return this.env.OPENAI_MODEL?.trim().slice(0, 100) || 'gpt-4.1-mini'; }
  private room(observation = this.state.latest) {
    if (!observation) throw new Error('No game observation.');
    return this.env.GAME_ROOM.get(this.env.GAME_ROOM.idFromString(observation.roomId));
  }

  async getStatus(): Promise<DirectorStatus> {
    // Refresh transport receipts without invoking OpenAI or triggering a run.
    const recent = this.state.actions.filter(a => a.tool === 'send_announcement' && (a.result.status === 'queued' || (a.result.status === 'started' && !this.state.activeRun))).slice(-3);
    for (const action of recent) {
      if (!this.state.latest) break;
      try {
        const result = await this.room().getDirectorAnnouncementResult(action.id);
        this.updateAction(action.id, { ...result });
      } catch { /* Keep the last known transport result when the game is unavailable. */ }
    }
    return { ...this.state, model: this.model(), enabled: this.enabled(), configured: this.configured(), limits: limits(this.env),
      ...(!this.enabled() || !this.configured() ? { status: 'disabled' as const,
        reason: !this.enabled() ? 'Director is disabled.' : 'OPENAI_API_KEY is not configured on the Worker.' } : {}) };
  }

  async observe(observation: DirectorObservation): Promise<void> {
    if (observation.gameId !== this.env.DIRECTOR_GAME_ID || observation.history.length > MAX_HISTORY) return;
    const previous = this.state.latest;
    if (previous && (observation.revision < previous.revision ||
      (observation.revision === previous.revision && observation.observedAt < previous.observedAt))) return;
    const changedRound = previous?.roundId !== observation.roundId;
    const trigger = meaningfulChange(previous, observation);
    const changed = !previous || observationKey(previous) !== observationKey(observation);
    const oldFollowUps = changedRound ? this.state.followUps : [];
    const oldWake = changedRound ? this.state.pending?.scheduleId : undefined;
    this.setState({ ...this.state, latest: observation, model: this.model(),
      observations: changed ? [...this.state.observations, observation].slice(-32) : this.state.observations,
      ...(changedRound ? { roundRequests: 0, followUps: [], pending: null } : {}) });
    if (oldWake) await this.cancelSchedule(oldWake);
    for (const follow of oldFollowUps) await this.cancelSchedule(follow.id);
    if (!this.enabled() || !this.configured()) {
      this.setState({ ...this.state, status: 'disabled', reason: !this.enabled() ? 'Director is disabled.' : 'OPENAI_API_KEY is not configured on the Worker.' });
      return;
    }
    if (!observation.roundId) {
      this.setState({ ...this.state, status: 'idle', reason: 'Waiting for a prepared live badge round.' });
      return;
    }
    if (trigger) await this.enqueue(trigger, observation.roundId);
  }

  private async enqueue(trigger: string, roundId: string): Promise<void> {
    if (this.state.latest?.roundId !== roundId) return;
    const oldWake = this.state.pending?.scheduleId;
    const dueAt = Math.max(Date.now() + 1000, this.state.lastRunAt + limits(this.env).cooldownMs,
      this.state.activeRun?.expiresAt ?? 0);
    this.setState({ ...this.state, pending: { roundId, trigger, dueAt, scheduleId: oldWake },
      status: this.state.activeRun ? 'thinking' : 'waiting', reason: `Pending ${trigger.replaceAll('_', ' ')}.` });
    await this.scheduleWake();
  }
  private async scheduleWake(): Promise<void> {
    const pending = this.state.pending;
    if (!pending) return;
    const scheduled = await this.schedule(new Date(Math.ceil(Math.max(Date.now() + 100, pending.dueAt) / 1000) * 1000), 'wake',
      { roundId: pending.roundId, dueAt: pending.dueAt }, { idempotent: true, retry: { maxAttempts: 1 } });
    if (this.state.pending?.roundId !== pending.roundId || this.state.pending.dueAt !== pending.dueAt) {
      await this.cancelSchedule(scheduled.id); return;
    }
    this.setState({ ...this.state, pending: { ...this.state.pending, scheduleId: scheduled.id } });
    if (pending.scheduleId && pending.scheduleId !== scheduled.id) await this.cancelSchedule(pending.scheduleId);
  }

  async wake(payload: Wake): Promise<void> {
    const pending = this.state.pending;
    if (!pending || pending.roundId !== payload.roundId || pending.dueAt !== payload.dueAt || this.state.latest?.roundId !== payload.roundId) return;
    if (!this.enabled() || !this.configured()) return;
    if (this.state.activeRun && this.state.activeRun.expiresAt > Date.now()) return;
    await this.runTurn(pending.trigger, payload.roundId);
  }

  async followUp(payload: FollowUp): Promise<void> {
    const saved = this.state.followUps.find(f => f.token === payload.id);
    if (!saved) return;
    this.setState({ ...this.state, followUps: this.state.followUps.filter(f => f.token !== payload.id) });
    if (!this.enabled() || this.state.latest?.roundId !== payload.roundId) return;
    try {
      const fresh = await this.room().getDirectorObservation();
      await this.observe(fresh);
      if (fresh.roundId === payload.roundId) await this.enqueue(`follow_up: ${payload.reason}`, payload.roundId);
    } catch {
      this.setState({ ...this.state, status: 'degraded', reason: 'Game unavailable at follow-up; gameplay continues.' });
    }
  }

  private reserveRequest(): boolean {
    const today = new Date().toISOString().slice(0, 10);
    const usage = this.state.dailyUsage.day === today ? this.state.dailyUsage : { day: today, requests: 0, inputTokens: 0, outputTokens: 0 };
    const cap = limits(this.env);
    if (usage.requests >= cap.dailyRequests || this.state.roundRequests >= cap.roundRequests) return false;
    this.setState({ ...this.state, dailyUsage: { ...usage, requests: usage.requests + 1 }, roundRequests: this.state.roundRequests + 1 });
    return true;
  }
  private updateRun(id: string, update: Partial<DirectorRun>): void {
    this.setState({ ...this.state, runs: this.state.runs.map(run => run.id === id ? { ...run, ...update } : run) });
  }
  private updateAction(id: string, result: Record<string, unknown>): void {
    this.setState({ ...this.state, actions: this.state.actions.map(action => action.id === id ? { ...action, result: JSON.parse(JSON.stringify(result)) as Record<string, JsonValue> } : action) });
  }
  private assertCurrent(runId: string, snapshot: DirectorObservation): void {
    if (this.state.activeRun?.id !== runId || Date.now() > this.state.activeRun.expiresAt ||
      this.state.latest?.roundId !== snapshot.roundId || this.state.latest.revision !== snapshot.revision) throw new StaleRun('Game changed during reasoning; actions discarded.');
  }

  private async runTurn(trigger: string, roundId: string): Promise<void> {
    const id = crypto.randomUUID(), now = Date.now();
    const run: DirectorRun = { id, at: now, trigger, roundId, status: 'running', model: this.model(),
      responseIds: [], requestIds: [], inputTokens: 0, outputTokens: 0, summary: '' };
    this.setState({ ...this.state, pending: null, activeRun: { id, roundId, expiresAt: now + MAX_RUN_MS },
      lastRunAt: now, status: 'thinking', reason: 'OpenAI is evaluating the current outbreak.', runs: [...this.state.runs, run].slice(-30) });
    try {
      const snapshot = await this.room().getDirectorObservation();
      if (snapshot.roundId !== roundId || this.state.latest?.roundId !== roundId ||
        this.state.latest.revision > snapshot.revision || this.state.latest.observedAt > snapshot.observedAt) throw new StaleRun('Game changed before reasoning.');
      // Freshness is checked again at every tool boundary and in the gateway.
      this.setState({ ...this.state, latest: snapshot });
      const context = { trigger, game: snapshot,
        memory: this.state.observations.slice(-6).map(o => ({ roundId: o.roundId, at: o.observedAt, infected: o.infected, humans: o.humans, phase: o.phase })),
        actions: this.state.actions.slice(-6), recaps: this.state.recaps.slice(-2), followUps: this.state.followUps };
      const input: ResponseInput = [{ role: 'user', content: JSON.stringify(context).slice(0, 24_000) }];
      const client = new OpenAI({ apiKey: this.env.OPENAI_API_KEY, maxRetries: 0, timeout: 12_000 });
      for (let step = 0; step < MAX_REQUESTS_PER_RUN; step++) {
        this.assertCurrent(id, snapshot);
        if (!this.reserveRequest()) {
          this.setState({ ...this.state, status: 'budget_exhausted', reason: 'Persistent request budget reached; gameplay continues.' });
          this.updateRun(id, { status: 'completed', summary: 'Stopped at request budget.' });
          return;
        }
        const response = await client.responses.create({ model: this.model(), instructions: DIRECTOR_INSTRUCTIONS, input,
          tools: DIRECTOR_TOOLS, tool_choice: step === MAX_REQUESTS_PER_RUN - 1 ? 'none' : 'auto', parallel_tool_calls: false,
          max_output_tokens: limits(this.env).outputTokens, store: false, include: ['reasoning.encrypted_content'] });
        const latestRun = this.state.runs.find(r => r.id === id)!;
        this.updateRun(id, { responseIds: [...latestRun.responseIds, response.id],
          requestIds: [...latestRun.requestIds, ...(response._request_id ? [response._request_id] : [])],
          inputTokens: latestRun.inputTokens + (response.usage?.input_tokens ?? 0),
          outputTokens: latestRun.outputTokens + (response.usage?.output_tokens ?? 0) });
        this.setState({ ...this.state, dailyUsage: { ...this.state.dailyUsage,
          inputTokens: this.state.dailyUsage.inputTokens + (response.usage?.input_tokens ?? 0),
          outputTokens: this.state.dailyUsage.outputTokens + (response.usage?.output_tokens ?? 0) } });
        this.assertCurrent(id, snapshot);
        if (response.status !== 'completed') throw new Error('OpenAI response was incomplete; no tools applied.');
        const calls = response.output.filter((item): item is ResponseFunctionToolCall => item.type === 'function_call');
        if (calls.length > 1 || (step === MAX_REQUESTS_PER_RUN - 1 && calls.length)) throw new Error('Model exceeded the tool-call limit.');
        if (!calls.length) {
          this.updateRun(id, { status: 'completed', summary: response.output_text.slice(0, 1000) || 'No action needed.' });
          this.setState({ ...this.state, status: 'idle', reason: 'Waiting for meaningful game events.' });
          return;
        }
        input.push(...toResponseInputItems(response.output));
        const result = await this.executeTool(id, snapshot, calls[0]);
        input.push({ type: 'function_call_output', call_id: calls[0].call_id, output: JSON.stringify(result) });
      }
    } catch (error) {
      const stale = error instanceof StaleRun;
      // Never persist provider error bodies or headers (may contain sensitive data).
      const reason = stale ? error.message : error instanceof OpenAI.APIError ? `OpenAI request failed (${error.status ?? 'network'}); waiting for a new event.`
        : error instanceof OpenAI.APIConnectionError ? 'OpenAI connection unavailable; waiting for a new event.' : 'Director run failed safely; waiting for a new event.';
      this.updateRun(id, { status: stale ? 'stale' : 'failed', summary: reason });
      this.setState({ ...this.state, status: stale ? 'waiting' : 'degraded', reason });
    } finally {
      if (this.state.activeRun?.id === id) this.setState({ ...this.state, activeRun: null });
      if (this.state.pending) {
        this.setState({ ...this.state, pending: { ...this.state.pending, dueAt: Math.max(Date.now() + 1000, this.state.lastRunAt + limits(this.env).cooldownMs) } });
        await this.scheduleWake();
      }
    }
  }

  private async executeTool(runId: string, snapshot: DirectorObservation, call: ResponseFunctionToolCall): Promise<Record<string, unknown>> {
    const id = `${runId}:${call.call_id}`;
    const existing = this.state.actions.find(a => a.id === id);
    if (existing) return existing.result;
    let args: Record<string, JsonValue>;
    try {
      if (call.arguments.length > 2048) throw new Error();
      const value: unknown = JSON.parse(call.arguments);
      if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error();
      const scalar = (v: unknown) => v === null || ['string', 'number', 'boolean'].includes(typeof v);
      if (Object.values(value).some(v => !scalar(v) && !(Array.isArray(v) && v.every(scalar)))) throw new Error();
      args = value as Record<string, JsonValue>;
    } catch { return { status: 'rejected', reason: 'Invalid tool arguments.' }; }
    this.assertCurrent(runId, snapshot);
    const action: DirectorAction = { id, at: Date.now(), roundId: snapshot.roundId!, tool: call.name, arguments: args,
      result: { status: 'started', reason: 'Tool intent persisted before execution.' } };
    this.setState({ ...this.state, actions: [...this.state.actions, action].slice(-80) });
    let result: Record<string, unknown>;
    try {
      const fresh = await this.room(snapshot).getDirectorObservation();
      if (fresh.roundId !== snapshot.roundId || fresh.revision !== snapshot.revision) throw new StaleRun('Game changed before tool execution.');
      this.assertCurrent(runId, snapshot);
      if (call.name === 'read_game_state') {
        result = { status: 'read', game: fresh };
      } else if (call.name === 'send_announcement') {
        result = { ...await this.room(snapshot).sendDirectorAnnouncement({ actionId: id, roundId: snapshot.roundId!, revision: snapshot.revision,
          text: cleanAnnouncement(args.text), expiresAt: Date.now() + 30_000 }) };
      } else if (call.name === 'schedule_follow_up') {
        const delay = args.delay_seconds;
        if (typeof delay !== 'number' || !Number.isInteger(delay) || delay < 15 || delay > 120 || typeof args.reason !== 'string' || args.reason.length > 120) throw new Error('Invalid follow-up.');
        if (fresh.endedAt || this.state.followUps.length >= 2) throw new Error('Follow-up unavailable after round end or when queue is full.');
        const token = crypto.randomUUID();
        const scheduled = await this.schedule(delay, 'followUp', { id: token, roundId: snapshot.roundId!, reason: args.reason }, { idempotent: true, retry: { maxAttempts: 1 } });
        try { this.assertCurrent(runId, snapshot); }
        catch (error) { await this.cancelSchedule(scheduled.id); throw error; }
        // The payload token and schedule cancellation ID are deliberately distinct.
        this.setState({ ...this.state, followUps: [...this.state.followUps, { id: scheduled.id, token, roundId: snapshot.roundId!, dueAt: Date.now() + delay * 1000, reason: args.reason }] });
        result = { status: 'scheduled', scheduleId: scheduled.id, dueAt: Date.now() + delay * 1000 };
      } else if (call.name === 'publish_recap') {
        const recap = groundedRecap(fresh, args, Date.now());
        const existing = this.state.recaps.find(r => r.roundId === recap.roundId);
        if (existing?.final && !recap.final) throw new Error('Cannot replace a final recap with a provisional result.');
        this.setState({ ...this.state, recaps: [...this.state.recaps.filter(r => r.roundId !== recap.roundId), recap].slice(-10) });
        result = { status: 'published', roundId: recap.roundId, final: recap.final, evidenceIds: recap.evidenceIds };
      } else result = { status: 'rejected', reason: 'Unknown tool.' };
    } catch (error) {
      result = { status: 'rejected', reason: error instanceof StaleRun ? error.message : 'Tool arguments or current game conditions do not permit this action.' };
    }
    // Do not retain duplicate full game histories in the action log.
    this.updateAction(id, call.name === 'read_game_state' && result.status === 'read' ? { status: 'read', revision: snapshot.revision } : result);
    return result;
  }
}
