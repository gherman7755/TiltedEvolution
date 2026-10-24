import { Injectable, NgZone, OnDestroy } from '@angular/core';
import { BehaviorSubject } from 'rxjs';
import { StoreService } from './store.service';

export interface MusicTrack {
  id: number;
  videoId: string;
  title: string;
  author: string;
  durationMs: number;
  addedById: number;
  addedByName: string;
}

export interface MusicSearchResult {
  videoId: string;
  title: string;
  author: string;
  durationMs: number;
}

export type MusicPlaybackState = 'idle' | 'preparing' | 'playing' | 'paused';

export interface MusicState {
  connected: boolean;
  state: MusicPlaybackState;
  hostPlayerId: number;
  localPlayerId: number;
  isHost: boolean;
  startServerTimeMs: number;
  positionMs: number;
  readyCount: number;
  playerCount: number;
  current: MusicTrack | null;
  queue: MusicTrack[];
}

export type MusicFileStatus = 'queued' | 'downloading' | 'ready' | 'failed';

export interface MusicFile {
  status: MusicFileStatus;
  progress: number;
  path: string;
  error: string;
}

export type MusicToolsStatus = 'missing' | 'installing' | 'ready' | 'failed';

export interface MusicSearch {
  query: string;
  loading: boolean;
  results: MusicSearchResult[];
  error: string;
}

/** Same values as MusicCommandRequest::Action in C++. */
export enum MusicCommand {
  ADD = 0,
  REMOVE,
  PLAY,
  NEXT,
  PAUSE,
  RESUME,
  SEEK,
  MOVE,
}

const EMPTY_STATE: MusicState = {
  connected: false,
  state: 'idle',
  hostPlayerId: 0,
  localPlayerId: 0,
  isHost: false,
  startServerTimeMs: 0,
  positionMs: 0,
  readyCount: 0,
  playerCount: 0,
  current: null,
  queue: [],
};

/** How often playback gets compared to the server clock. */
const SYNC_INTERVAL_MS = 50;
/** Further off than this and the audio jumps to the right spot instead of catching up smoothly. */
const SEEK_THRESHOLD_MS = 250;
/** Closer than this counts as in sync. */
const IN_SYNC_MS = 4;
/** Catching up removes the offset over about this long. */
const CATCH_UP_MS = 1500;
const MAX_RATE_CHANGE = 0.06;
/** A (re)start seeks a bit ahead of the server position and starts playing once the server clock gets there. */
const START_LEAD_MS = 400;
const MAX_START_LEAD_MS = 3000;

/**
 * Plays the shared music, in sync with every other player.
 *
 * The server says at which time on its clock the current track was at 0. The C++ side keeps
 * the offset between the local clock and the server clock up to date, so the position every
 * player should be at is known at any time. The audio jumps there when it is far off and
 * speeds up or slows down slightly to remove small differences.
 */
@Injectable({
  providedIn: 'root',
})
export class MusicService implements OnDestroy {
  public readonly state = new BehaviorSubject<MusicState>(EMPTY_STATE);
  public readonly files = new BehaviorSubject<Map<string, MusicFile>>(new Map());
  public readonly tools = new BehaviorSubject<{ status: MusicToolsStatus; error: string }>({ status: 'missing', error: '' });
  public readonly search = new BehaviorSubject<MusicSearch>({ query: '', loading: false, results: [], error: '' });
  /** Position of the current track in ms, for the progress bar. */
  public readonly position = new BehaviorSubject<number>(0);
  public readonly playbackError = new BehaviorSubject<string>('');

  public readonly volume = new BehaviorSubject<number>(this.store.getFloat('music_volume', 0.5));
  public readonly enabled = new BehaviorSubject<boolean>(this.store.getBool('music_enabled', true));
  /** Plays this much earlier to make up for slow audio devices, like Bluetooth headphones. */
  public readonly latencyMs = new BehaviorSubject<number>(this.store.getFloat('music_latency', 0));

  private readonly audio = new Audio();
  private clockOffsetMs = 0;
  private hasClock = false;

  private loadedTrackId = 0;
  private loadedPath = '';
  private loadToken = 0;
  private isLoaded = false;
  private loadFailed = false;
  private reportedTrackId = 0;

  /** Bumped whenever a scheduled start has to be thrown away. */
  private startToken = 0;
  private isStarting = false;
  private startLeadMs = START_LEAD_MS;
  private smoothedErrorMs = 0;
  private syncTimer: ReturnType<typeof setInterval>;

  constructor(
    private readonly zone: NgZone,
    private readonly store: StoreService,
  ) {
    this.audio.preload = 'auto';
    this.audio.preservesPitch = true;
    this.applyVolume();

    skyrimtogether.on('musicState', this.onState.bind(this));
    skyrimtogether.on('musicClock', this.onClock.bind(this));
    skyrimtogether.on('musicFile', this.onFile.bind(this));
    skyrimtogether.on('musicTools', this.onTools.bind(this));
    skyrimtogether.on('musicSearchResults', this.onSearchResults.bind(this));

    this.volume.subscribe(volume => {
      this.store.set('music_volume', volume);
      this.applyVolume();
    });
    this.enabled.subscribe(enabled => {
      this.store.set('music_enabled', enabled);
      if (!enabled) {
        this.stopAudio();
      }
    });
    this.latencyMs.subscribe(latency => {
      this.store.set('music_latency', latency);
      this.restart();
    });

    // Outside of Angular, so that the sync loop does not run change detection 20 times a second.
    this.zone.runOutsideAngular(() => {
      this.syncTimer = setInterval(() => this.sync(), SYNC_INTERVAL_MS);
    });

    // The UI may have been reloaded, get everything again.
    skyrimtogether.musicSync();
  }

  public ngOnDestroy(): void {
    clearInterval(this.syncTimer);
    skyrimtogether.off('musicState');
    skyrimtogether.off('musicClock');
    skyrimtogether.off('musicFile');
    skyrimtogether.off('musicTools');
    skyrimtogether.off('musicSearchResults');
    this.stopAudio();
  }

  public searchTracks(query: string): void {
    query = query.trim();
    if (!query) {
      return;
    }
    this.search.next({ query, loading: true, results: [], error: '' });
    skyrimtogether.musicSearch(query);
  }

  public addTrack(track: MusicSearchResult): void {
    skyrimtogether.musicAdd(track.videoId, track.title, track.author, track.durationMs);
  }

  public command(command: MusicCommand, trackId = 0, value = 0): void {
    skyrimtogether.musicCommand(command, trackId, value);
  }

  public installTools(): void {
    skyrimtogether.musicInstallTools();
  }

  public setEnabled(enabled: boolean): void {
    this.enabled.next(enabled);

    // Tell the server right away, so that nobody waits for a player who does not listen,
    // and so that a player who turns the music back on counts again.
    const current = this.state.value.current;
    if (!current) {
      return;
    }
    if (!enabled) {
      this.reportedTrackId = current.id;
      skyrimtogether.musicTrackLoaded(current.id, false, 0);
    } else if (this.isLoaded && this.loadedTrackId === current.id) {
      this.reportedTrackId = current.id;
      skyrimtogether.musicTrackLoaded(current.id, true, this.durationMs());
    } else {
      this.reportedTrackId = 0;
    }
  }

  /** The server clock in ms. */
  private serverNow(): number {
    return Date.now() + this.clockOffsetMs;
  }

  /** Where the current track should be right now, in ms. */
  private targetMs(): number {
    const state = this.state.value;
    if (state.state !== 'playing') {
      return state.positionMs;
    }
    return this.serverNow() - state.startServerTimeMs + this.latencyMs.value;
  }

  private durationMs(): number {
    const duration = this.audio.duration;
    return isFinite(duration) ? Math.round(duration * 1000) : 0;
  }

  private onState(state: MusicState): void {
    const previous = this.state.value;
    const changedTiming =
      state.state !== previous.state ||
      state.startServerTimeMs !== previous.startServerTimeMs ||
      state.positionMs !== previous.positionMs ||
      state.current?.id !== previous.current?.id;

    this.zone.run(() => this.state.next(state));

    if (changedTiming) {
      this.restart();
    }
  }

  private onClock(offsetMs: number): void {
    this.clockOffsetMs = offsetMs;
    this.hasClock = true;
  }

  private onFile(videoId: string, status: MusicFileStatus, progress: number, path: string, error: string): void {
    this.zone.run(() => {
      const files = new Map(this.files.value);
      files.set(videoId, { status, progress, path, error });
      this.files.next(files);
    });
  }

  private onTools(status: MusicToolsStatus, error: string): void {
    this.zone.run(() => this.tools.next({ status, error }));
  }

  private onSearchResults(query: string, results: MusicSearchResult[], error: string): void {
    this.zone.run(() => this.search.next({ query, loading: false, results, error }));
  }

  private applyVolume(): void {
    // Perceived loudness is closer to the square of the slider.
    const volume = Math.min(Math.max(this.volume.value, 0), 1);
    this.audio.volume = volume * volume;
  }

  /** Throws away any scheduled start, the next sync starts again from the current state. */
  private restart(): void {
    this.startToken++;
    this.isStarting = false;
    this.startLeadMs = START_LEAD_MS;
    this.smoothedErrorMs = 0;
  }

  private stopAudio(): void {
    this.restart();
    if (!this.audio.paused) {
      this.audio.pause();
    }
  }

  private unload(): void {
    this.stopAudio();
    this.loadToken++;
    this.loadedTrackId = 0;
    this.loadedPath = '';
    this.isLoaded = false;
    this.loadFailed = false;
    this.audio.removeAttribute('src');
    this.audio.load();
  }

  private load(track: MusicTrack, path: string): void {
    this.unload();

    const token = ++this.loadToken;
    this.loadedTrackId = track.id;
    this.loadedPath = path;
    this.zone.run(() => this.playbackError.next(''));

    const onReady = () => {
      if (token !== this.loadToken || this.isLoaded) {
        return;
      }
      this.isLoaded = true;
      this.report(track.id, true);
    };
    const onError = () => {
      if (token !== this.loadToken) {
        return;
      }
      this.loadFailed = true;
      const code = this.audio.error?.code ?? 0;
      this.zone.run(() => this.playbackError.next(`MEDIA_ERR_${code}`));
      this.report(track.id, false);
    };

    this.audio.addEventListener('canplaythrough', onReady, { once: true });
    this.audio.addEventListener('error', onError, { once: true });
    this.audio.src = toFileUrl(path);
    this.audio.load();
  }

  private report(trackId: number, ready: boolean): void {
    if (this.reportedTrackId === trackId) {
      return;
    }
    this.reportedTrackId = trackId;
    skyrimtogether.musicTrackLoaded(trackId, ready && this.enabled.value, this.durationMs());
  }

  private sync(): void {
    const state = this.state.value;

    if (!state.connected || state.state === 'idle' || !state.current) {
      if (this.loadedTrackId) {
        this.unload();
      }
      this.updatePosition(0);
      return;
    }

    this.updatePosition(Math.max(0, Math.min(this.targetMs(), state.current.durationMs || Infinity)));

    if (!this.enabled.value) {
      this.report(state.current.id, false);
      return;
    }

    const file = this.files.value.get(state.current.videoId);
    if (!file || file.status !== 'ready') {
      this.stopAudio();
      return;
    }

    if (this.loadedTrackId !== state.current.id || this.loadedPath !== file.path) {
      this.load(state.current, file.path);
      return;
    }

    if (!this.isLoaded || this.loadFailed || !this.enabled.value) {
      return;
    }

    if (state.state !== 'playing' || !this.hasClock) {
      // Sit at the right spot so that resuming is instant.
      this.stopAudio();
      if (Math.abs(this.audio.currentTime * 1000 - state.positionMs) > 50 && !this.audio.seeking) {
        this.audio.currentTime = state.positionMs / 1000;
      }
      return;
    }

    if (this.isStarting) {
      return;
    }

    const target = this.targetMs();
    const duration = this.durationMs() || state.current.durationMs;
    if (duration && target >= duration) {
      this.stopAudio();
      return;
    }

    if (this.audio.paused || this.audio.seeking) {
      void this.startAt(target);
      return;
    }

    const errorMs = this.audio.currentTime * 1000 - target;
    if (Math.abs(errorMs) > SEEK_THRESHOLD_MS) {
      void this.startAt(target);
      return;
    }

    // currentTime moves in small steps, smooth it a little so the rate does not jitter.
    this.smoothedErrorMs = this.smoothedErrorMs * 0.6 + errorMs * 0.4;

    let rate = 1;
    if (Math.abs(this.smoothedErrorMs) > IN_SYNC_MS) {
      const change = Math.max(-MAX_RATE_CHANGE, Math.min(MAX_RATE_CHANGE, this.smoothedErrorMs / CATCH_UP_MS));
      rate = 1 - change;
    }
    if (Math.abs(this.audio.playbackRate - rate) > 0.001) {
      this.audio.playbackRate = rate;
    }
  }

  /**
   * Seeks a little ahead of where the track should be and starts playing exactly when the server clock gets there.
   * Starting right away would be late by however long the seek takes.
   */
  private async startAt(targetMs: number): Promise<void> {
    const token = ++this.startToken;
    this.isStarting = true;
    this.smoothedErrorMs = 0;

    this.audio.pause();
    this.audio.playbackRate = 1;

    const startMs = Math.max(0, targetMs + this.startLeadMs);
    this.audio.currentTime = startMs / 1000;
    await waitForSeek(this.audio);

    if (token !== this.startToken) {
      return;
    }

    const delayMs = startMs - this.targetMs();
    if (delayMs < 10) {
      // The seek took longer than the lead, aim further ahead next time.
      this.startLeadMs = Math.min(this.startLeadMs * 2, MAX_START_LEAD_MS);
      this.isStarting = false;
      return;
    }

    setTimeout(() => {
      if (token !== this.startToken) {
        return;
      }
      this.isStarting = false;
      this.audio.play().catch(() => {
        if (token === this.startToken) {
          this.zone.run(() => this.playbackError.next('PLAY_BLOCKED'));
        }
      });
    }, delayMs);
  }

  private updatePosition(positionMs: number): void {
    // Only for display, a few updates a second are enough.
    if (Math.abs(positionMs - this.position.value) >= 250 || positionMs === 0) {
      if (positionMs !== this.position.value) {
        this.zone.run(() => this.position.next(positionMs));
      }
    }
  }
}

function waitForSeek(audio: HTMLAudioElement): Promise<void> {
  if (!audio.seeking) {
    return Promise.resolve();
  }
  return new Promise(resolve => {
    const done = () => {
      clearTimeout(timeout);
      audio.removeEventListener('seeked', done);
      resolve();
    };
    const timeout = setTimeout(done, 3000);
    audio.addEventListener('seeked', done);
  });
}

function toFileUrl(path: string): string {
  const parts = path.split(/[\\/]/);
  return (
    'file:///' +
    parts
      .map((part, index) => (index === 0 && /^[a-zA-Z]:$/.test(part) ? part : encodeURIComponent(part)))
      .join('/')
  );
}
