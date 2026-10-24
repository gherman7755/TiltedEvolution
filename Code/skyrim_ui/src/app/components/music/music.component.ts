import {
  ChangeDetectionStrategy,
  Component,
  EventEmitter,
  HostListener,
  Output,
} from '@angular/core';
import { combineLatest, map } from 'rxjs';
import {
  MusicCommand,
  MusicSearchResult,
  MusicService,
  MusicTrack,
} from '../../services/music.service';
import { Sound, SoundService } from '../../services/sound.service';

export enum MusicTab {
  PLAYER,
  YOUTUBE,
}

@Component({
  selector: 'app-music',
  templateUrl: './music.component.html',
  styleUrls: ['./music.component.scss'],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class MusicComponent {
  /* ### ENUMS ### */
  readonly MusicTab = MusicTab;
  readonly MusicCommand = MusicCommand;

  @Output() public done = new EventEmitter<void>();

  tab: MusicTab = MusicTab.PLAYER;
  query = this.music.search.value.query;

  state$ = this.music.state;
  search$ = this.music.search;
  tools$ = this.music.tools;
  position$ = this.music.position;
  playbackError$ = this.music.playbackError;
  volume$ = this.music.volume;
  enabled$ = this.music.enabled;
  latency$ = this.music.latencyMs;

  /** Download state of the current track. */
  currentFile$ = combineLatest([this.music.state, this.music.files]).pipe(
    map(([state, files]) => (state.current ? files.get(state.current.videoId) : undefined)),
  );

  /** Video ids already in the queue, so that the search results can show them. */
  queuedIds$ = this.music.state.pipe(
    map(state => new Set([...(state.current ? [state.current.videoId] : []), ...state.queue.map(track => track.videoId)])),
  );

  constructor(
    private readonly music: MusicService,
    private readonly sound: SoundService,
  ) {}

  switchTab(tab: MusicTab) {
    this.tab = tab;
    this.sound.play(Sound.Focus);
  }

  submitSearch() {
    this.music.searchTracks(this.query);
    this.sound.play(Sound.Ok);
  }

  add(result: MusicSearchResult) {
    this.music.addTrack(result);
    this.sound.play(Sound.Ok);
  }

  command(command: MusicCommand, track?: MusicTrack, value = 0) {
    this.music.command(command, track?.id ?? 0, value);
    this.sound.play(Sound.Focus);
  }

  seek(event: MouseEvent, durationMs: number) {
    const bar = event.currentTarget as HTMLElement;
    const ratio = (event.clientX - bar.getBoundingClientRect().left) / bar.clientWidth;
    const current = this.music.state.value.current;
    if (current && durationMs > 0) {
      this.music.command(MusicCommand.SEEK, current.id, Math.max(0, Math.min(1, ratio)) * durationMs);
    }
  }

  move(track: MusicTrack, index: number) {
    this.command(MusicCommand.MOVE, track, index);
  }

  setVolume(volume: number) {
    this.music.volume.next(Number(volume));
  }

  setLatency(latency: number) {
    this.music.latencyMs.next(Number(latency));
  }

  toggleEnabled() {
    this.music.setEnabled(!this.music.enabled.value);
    this.sound.play(Sound.Focus);
  }

  installTools() {
    this.music.installTools();
    this.sound.play(Sound.Ok);
  }

  canRemove(track: MusicTrack, isHost: boolean, localPlayerId: number): boolean {
    return isHost || track.addedById === localPlayerId;
  }

  formatDuration(ms: number): string {
    return ms > 0 ? this.formatTime(ms) : '-:--';
  }

  formatTime(ms: number): string {
    if (!(ms > 0) || !isFinite(ms)) {
      ms = 0;
    }
    const totalSeconds = Math.floor(ms / 1000);
    const hours = Math.floor(totalSeconds / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = (totalSeconds % 60).toString().padStart(2, '0');
    return hours > 0 ? `${hours}:${minutes.toString().padStart(2, '0')}:${seconds}` : `${minutes}:${seconds}`;
  }

  trackById(_: number, track: MusicTrack): number {
    return track.id;
  }

  resultById(_: number, result: MusicSearchResult): string {
    return result.videoId;
  }

  close() {
    this.done.next();
    this.sound.play(Sound.Ok);
  }

  @HostListener('window:keydown.escape', ['$event'])
  // @ts-ignore
  private activate(event: KeyboardEvent): void {
    this.close();
    event.stopPropagation();
    event.preventDefault();
  }
}
