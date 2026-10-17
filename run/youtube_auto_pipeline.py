#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
YouTube Auto-Pipeline
=====================
Tu dong hoa toan bo quy trinh: render video game (theo duration audio)
-> mix audio + nhac nen -> concat cac chuong -> upload YouTube -> don dep file tam.

Su dung:
  1. CLI: python youtube_auto_pipeline.py --chapters ./chapters ...
  2. Python API:
     from youtube_auto_pipeline import Pipeline, PipelineConfig, TemplateConfig
     config = PipelineConfig(...)
     pipeline = Pipeline(config)
     pipeline.run()
"""

import os
import argparse
import logging
import math
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from typing import List, Optional
from dataclasses import dataclass, field
import wave

# Setup logging
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s"
)
logger = logging.getLogger(__name__)

# Import YouTubeUploader tu module ben ngoai
try:
    from youtube_uploader import YouTubeUploader
except ImportError:
    logger.error(
        "Khong tim thay module 'youtube_uploader'. "
        "Vui long dam bao file youtube_uploader.py nam trong PYTHONPATH hoac cung thu muc."
    )
    sys.exit(1)


__all__ = [
    "TemplateConfig",
    "PipelineConfig",
    "ChapterGroup",
    "AudioProcessor",
    "VideoRenderer",
    "AudioMixer",
    "CleanupManager",
    "Pipeline",
    "build_config_from_args",
]


@dataclass
class TemplateConfig:
    """Cau hinh template cho video upload."""
    title_pattern: str
    description: str
    tags: List[str]
    category_id: str
    privacy_status: str
    playlist_id: Optional[str] = None


@dataclass
class PipelineConfig:
    """Cau hinh toan bo pipeline."""
    chapters_dir: Path
    temp_dir: Path = field(default_factory=lambda: Path("./temp"))
    output_dir: Path = field(default_factory=lambda: Path("./output"))
    chapter_output_dir: Optional[Path] = None  # folder output tung chuong (mac dinh: temp_dir/chapters)
    template: Optional[TemplateConfig] = None
    start_index: int = 1
    max_duration_per_video: float = 12 * 3600
    background_music: Optional[Path] = None
    bg_music_volume: float = 0.15
    bg_music_loop: bool = False  # Lap lai nhac nen, co fade in/out 5s
    keep_output: bool = False
    game_path: Optional[str] = None
    game_args: Optional[str] = None
    info_video: Optional[Path] = None


@dataclass
class ChapterGroup:
    """Mot nhom chapter duoc ghep thanh 1 video."""
    index: int
    audio_files: List[Path] = field(default_factory=list)
    total_duration: float = 0.0


class AudioProcessor:
    """Xu ly ghep va phan tich audio."""

    @staticmethod
    def get_wav_duration(file_path: Path) -> float:
        try:
            with wave.open(str(file_path), "rb") as wf:
                frames = wf.getnframes()
                rate = wf.getframerate()
                return frames / float(rate)
        except Exception as e:
            logger.error(f"Khong doc duoc duration cua {file_path}: {e}")
            return 0.0

    @staticmethod
    def merge_wav_files(input_files: List[Path], output_file: Path) -> bool:
        if not input_files:
            return False
        try:
            concat_file = output_file.parent / f"concat_list_{os.getpid()}.txt"
            with open(concat_file, "w", encoding="utf-8") as f:
                for wav_file in input_files:
                    escaped_path = str(wav_file.absolute()).replace("\\", "/").replace("'", "'\\''")
                    f.write(f"file '{escaped_path}'\n")

            cmd = [
                "ffmpeg", "-y", "-f", "concat", "-safe", "0",
                "-i", str(concat_file),
                "-acodec", "pcm_s16le", "-ar", "44100", "-ac", "2",
                str(output_file)
            ]
            subprocess.run(cmd, capture_output=True, text=True, check=True)
            concat_file.unlink(missing_ok=True)
            print(f"Da ghep audio ({len(input_files)} files): {output_file}")
            return True
        except subprocess.CalledProcessError as e:
            logger.error(f"Loi ghep audio: {e.stderr}")
            return False
        except Exception as e:
            logger.error(f"Loi khong xac dinh khi ghep audio: {e}")
            return False


class VideoRenderer:
    """Render video tu duration. Ho tro game hoac ffmpeg fallback."""

    def __init__(self, game_path: Optional[str] = None, game_args: Optional[str] = None):
        self.game_path = game_path or "flappy_bird"
        self.game_args = game_args or '"{game_path}" --duration {duration} --fps 24 --flap -7.0 -o "{output_path}"'

    @staticmethod
    def get_video_duration(file_path: Path) -> float:
        try:
            cmd = [
                "ffprobe", "-v", "error", "-show_entries",
                "format=duration", "-of",
                "default=noprint_wrappers=1:nokey=1", str(file_path)
            ]
            result = subprocess.run(cmd, capture_output=True, text=True, check=True)
            return float(result.stdout.strip())
        except Exception as e:
            logger.error(f"Khong doc duoc duration cua {file_path}: {e}")
            return 0.0

    def render(self, duration: float, output_path: Path) -> bool:
        """Render video voi duration (giay) lay tu file audio."""
        try:
            if self._check_game():
                return self._render_with_game(duration, output_path)
            else:
                return self._render_with_ffmpeg(duration, output_path)
        except Exception as e:
            logger.error(f"Loi render video: {e}")
            return False

    def _check_game(self) -> bool:
        try:
            subprocess.run([self.game_path, "--help"], capture_output=True, check=True)
            return True
        except (subprocess.CalledProcessError, FileNotFoundError):
            print(f"Game '{self.game_path}' khong kha dung, su dung ffmpeg fallback")
            return False

    def _render_with_game(self, duration: float, output_path: Path) -> bool:
        cmd_str = self.game_args.format(
            game_path=Path(self.game_path).as_posix() if self.game_path else "",
            duration=duration,
            output_path=output_path.as_posix()
        )
        cmd = shlex.split(cmd_str)
        subprocess.run(cmd, capture_output=True, text=True, check=True)
        print(f"Da render video bang game: {output_path}")
        return True

    def _render_with_ffmpeg(self, duration: float, output_path: Path) -> bool:
        cmd = [
            "ffmpeg", "-y",
            "-f", "lavfi", "-i", f"color=c=black:s=1920x1080:r=30:d={duration}",
            "-c:v", "libx264", "-preset", "medium", "-crf", "23",
            "-pix_fmt", "yuv420p", "-an",
            str(output_path)
        ]
        subprocess.run(cmd, capture_output=True, text=True, check=True)
        print(f"Da render video bang ffmpeg (duration={duration:.2f}s): {output_path}")
        return True


class AudioMixer:
    """Ghep audio chinh va nhac nen vao video. Ho tro concat nhieu video."""

    @staticmethod
    def mix_audio(
        main_audio: Path,
        video_input: Path,
        output_path: Path,
        background_music: Optional[Path] = None,
        bg_volume: float = 0.15,
        audio_duration: float = 0.0,
        bg_music_loop: bool = False
    ) -> bool:
        """
        Ghep audio chinh vao video, tuy chon mix nhac nen.

        - bg_music_loop=False: nhac nen phat 1 lan tu dau chuong, khong lap lai.
        - bg_music_loop=True:  nhac nen lap vo han, fade in 5s dau + fade out 5s cuoi chuong.
        """
        try:
            if background_music and background_music.exists():
                if bg_music_loop:
                    # Tinh fade duration an toan (tranh chong lan khi audio qua ngan)
                    fade_duration = min(5.0, audio_duration / 2.0) if audio_duration > 0 else 5.0
                    fade_out_start = max(0.0, audio_duration - fade_duration)

                    filter_complex = (
                        f"[2:a]aloop=loop=-1:size=0,volume={bg_volume},"
                        f"afade=t=in:d={fade_duration},"
                        f"afade=t=out:st={fade_out_start}:d={fade_duration}"
                        f"[bg];[1:a][bg]amix=inputs=2:duration=first:dropout_transition=3[aout]"
                    )
                    cmd = [
                        "ffmpeg", "-y",
                        "-i", str(video_input), "-i", str(main_audio),
                        "-stream_loop", "-1", "-i", str(background_music),
                        "-filter_complex", filter_complex,
                        "-map", "0:v", "-map", "[aout]",
                        "-c:v", "copy", "-c:a", "aac", "-b:a", "192k",
                        str(output_path)
                    ]
                else:
                    # Khong loop: nhac nen phat 1 lan tu dau, khong lap lai
                    cmd = [
                        "ffmpeg", "-y",
                        "-i", str(video_input), "-i", str(main_audio),
                        "-i", str(background_music),
                        "-filter_complex",
                        f"[2:a]volume={bg_volume}[bg];[1:a][bg]amix=inputs=2:duration=first:dropout_transition=3[aout]",
                        "-map", "0:v", "-map", "[aout]",
                        "-c:v", "copy", "-c:a", "aac", "-b:a", "192k",
                        str(output_path)
                    ]
            else:
                cmd = [
                    "ffmpeg", "-y",
                    "-i", str(video_input), "-i", str(main_audio),
                    "-c:v", "copy", "-c:a", "aac", "-b:a", "192k",
                    "-map", "0:v:0", "-map", "1:a:0", "-shortest",
                    str(output_path)
                ]
            subprocess.run(cmd, capture_output=True, text=True, check=True)
            print(f"Da mix audio: {output_path}")
            return True
        except subprocess.CalledProcessError as e:
            logger.error(f"Loi mix audio: {e.stderr}")
            return False
        except Exception as e:
            logger.error(f"Loi khong xac dinh khi mix audio: {e}")
            return False

    @staticmethod
    def concat_videos(
        video_list: List[Path],
        output_path: Path,
        temp_dir: Path
    ) -> bool:
        """
        Concat nhieu video thanh 1 bang phuong phap copy (khong re-encode).
        """
        if not video_list:
            return False

        concat_file = temp_dir / f"concat_list_{os.getpid()}.txt"
        try:
            # Tao concat list
            with open(concat_file, "w", encoding="utf-8") as f:
                for v in video_list:
                    escaped = str(v.absolute()).replace("\\", "/")
                    f.write(f"file '{escaped}'\n")

            cmd_concat = [
                "ffmpeg", "-y",
                "-f", "concat", "-safe", "0",
                "-i", str(concat_file),
                "-c", "copy",
                str(output_path),
            ]
            subprocess.run(cmd_concat, capture_output=True, text=True, check=True)
            print(f"Da concat {len(video_list)} video: {output_path}")
            return True

        except subprocess.CalledProcessError as e:
            logger.error(f"Loi concat video: {e.stderr}")
            return False
        except Exception as e:
            logger.error(f"Loi khong xac dinh khi concat video: {e}")
            return False
        finally:
            concat_file.unlink(missing_ok=True)

    @staticmethod
    def prepend_intro(
        intro_path: Path,
        main_video: Path,
        output_path: Path,
        temp_dir: Path,
    ) -> bool:
        """
        Noi intro.mp4 len dau main_video bang phuong phap copy (khong re-encode).
        Buoc 1: Them silent audio vao intro neu chua co (de dong bo stream voi main video).
        Buoc 2: Concat bang -c copy.
        """
        intro_with_audio = temp_dir / f"intro_a_{os.getpid()}.mp4"
        concat_list      = temp_dir / f"concat_list_intro_{os.getpid()}.txt"
        try:
            # Buoc 1 – Gan silent audio vao intro
            cmd_intro = [
                "ffmpeg", "-y",
                "-i", str(intro_path),
                "-f", "lavfi", "-i", "anullsrc=r=44100:cl=stereo",
                "-map", "0:v:0", "-map", "1:a:0",
                "-c:v", "copy",
                "-c:a", "aac", "-b:a", "192k",
                "-shortest",
                str(intro_with_audio),
            ]
            subprocess.run(cmd_intro, capture_output=True, text=True, check=True)
            print(f"Da them silent audio vao intro: {intro_with_audio}")

            # Buoc 2 – Tao concat list roi ghep
            with open(concat_list, "w", encoding="utf-8") as f:
                f.write(f"file '{intro_with_audio.absolute()}'\n")
                f.write(f"file '{main_video.absolute()}'\n")

            cmd_concat = [
                "ffmpeg", "-y",
                "-f", "concat", "-safe", "0",
                "-i", str(concat_list),
                "-c", "copy",
                str(output_path),
            ]
            subprocess.run(cmd_concat, capture_output=True, text=True, check=True)
            print(f"Da noi intro vao dau video: {output_path}")
            return True

        except subprocess.CalledProcessError as e:
            logger.error(f"Loi noi intro: {e.stderr}")
            return False
        except Exception as e:
            logger.error(f"Loi khong xac dinh khi noi intro: {e}")
            return False
        finally:
            intro_with_audio.unlink(missing_ok=True)
            concat_list.unlink(missing_ok=True)


class CleanupManager:
    """Quan ly don dep file tam. Chi xoa sau khi upload thanh cong."""

    def __init__(self, keep_output: bool = False):
        self.keep_output = keep_output
        self.temp_files: List[Path] = []

    def register(self, file_path: Path):
        self.temp_files.append(file_path)

    def cleanup(self, success: bool = True):
        if not success:
            print("Upload that bai, giu lai file de debug/retry")
            return
        for file_path in self.temp_files:
            try:
                if file_path.exists():
                    if file_path.is_file():
                        file_path.unlink()
                        print(f"Da xoa file: {file_path}")
                    elif file_path.is_dir():
                        shutil.rmtree(file_path)
                        print(f"Da xoa thu muc: {file_path}")
            except Exception as e:
                logger.warning(f"Khong the xoa {file_path}: {e}")
        self.temp_files.clear()


class Pipeline:
    """Pipeline chinh dieu phoi toan bo quy trinh."""

    def __init__(self, config: PipelineConfig):
        self.config = config
        self.audio_processor = AudioProcessor()
        self.video_renderer = VideoRenderer(
            game_path=config.game_path,
            game_args=config.game_args,
        )
        self.audio_mixer = AudioMixer()
        self.uploader = YouTubeUploader()

        self.config.temp_dir.mkdir(parents=True, exist_ok=True)
        self.config.output_dir.mkdir(parents=True, exist_ok=True)

        # Tao chapter_output_dir neu chua co
        if self.config.chapter_output_dir is None:
            self.config.chapter_output_dir = self.config.temp_dir / "chapters"
        self.config.chapter_output_dir.mkdir(parents=True, exist_ok=True)

    def analyze_data(self) -> List[Path]:
        """Scan audio files trong chapters_dir."""
        audio_files = sorted(self.config.chapters_dir.glob("chapter_*.wav"))

        if not audio_files:
            raise ValueError(f"Khong tim thay file audio trong {self.config.chapters_dir}")

        print(f"Tim thay {len(audio_files)} file audio")
        return audio_files

    def split_into_groups(self, audio_files: List[Path]) -> List[ChapterGroup]:
        """Chia audio files thanh cac nhom theo max_duration_per_video."""
        durations = [self.audio_processor.get_wav_duration(f) for f in audio_files]
        total_duration = sum(durations)

        num_groups = max(1, math.ceil(total_duration / self.config.max_duration_per_video))
        target_per_group = total_duration / num_groups

        print(f"Tong duration: {total_duration/3600:.2f}h -> chia thanh {num_groups} group, moi group ~{target_per_group/3600:.2f}h")

        groups = []
        current_group = ChapterGroup(index=self.config.start_index)

        for audio, duration in zip(audio_files, durations):
            # Neu group hien tai da du target VA con chapter phia sau -> flush
            if (current_group.total_duration + duration > target_per_group
                    and current_group.audio_files
                    and len(groups) < num_groups - 1):   # giu lai slot cho group cuoi
                groups.append(current_group)
                current_group = ChapterGroup(index=current_group.index + 1)

            current_group.audio_files.append(audio)
            current_group.total_duration += duration

        if current_group.audio_files:
            groups.append(current_group)

        print(f"Da chia thanh {len(groups)} nhom:")
        for g in groups:
            print(f"  Video {g.index}: {len(g.audio_files)} chapters, {g.total_duration/3600:.2f}h")
        return groups

    def process_group(self, group: ChapterGroup, total_groups: int) -> bool:
        """
        Xu ly 1 nhom chapter theo luong:
        1. Ghep tat ca audio thanh 1 file lon.
        2. Render video game tung chuong (de ho tro resume) -> ghep cac video nguyen ban nay thanh 1 file video raw lon.
        3. Mix video raw lon voi audio lon (+ nhac nen). Nhac nen se lien mach ca video.
        4. Noi intro vao dau video da mix.
        5. Upload YouTube.
        """
        cleanup = CleanupManager(keep_output=self.config.keep_output)
        template = self.config.template
        if template is None:
            raise ValueError("PipelineConfig.template khong duoc de None")

        chapter_output_dir = self.config.chapter_output_dir
        num_chapters = len(group.audio_files)

        try:
            # ── Buoc 1: Ghep Audio ───────────────────────────────────────
            merged_audio = self.config.temp_dir / f"merged_audio_{group.index}.wav"
            cleanup.register(merged_audio)
            
            print(f"[Video {group.index}] Buoc 1: Ghep {num_chapters} audio files...")
            if not self.audio_processor.merge_wav_files(group.audio_files, merged_audio):
                raise RuntimeError("Ghep audio that bai")
            
            total_audio_duration = self.audio_processor.get_wav_duration(merged_audio)

            # ── Buoc 2: Render tung video va Ghep Video Raw ─────────────────
            chapter_raw_videos: List[Path] = []
            
            print(f"[Video {group.index}] Buoc 2: Render video cho {num_chapters} chuong...")
            for i, audio_file in enumerate(group.audio_files):
                chapter_num = i + 1
                duration = self.audio_processor.get_wav_duration(audio_file)
                chapter_raw = chapter_output_dir / f"group{group.index}_ch{chapter_num}_raw.mp4"
                
                # Resume logic
                if chapter_raw.exists():
                    video_duration = self.video_renderer.get_video_duration(chapter_raw)
                    if abs(video_duration - duration) < 1.0:
                        print(f"  -> Chuong {chapter_num}: Bo qua render (da ton tai va duration hop le: {video_duration:.2f}s)")
                        cleanup.register(chapter_raw)
                        chapter_raw_videos.append(chapter_raw)
                        continue
                    else:
                        print(f"  -> Chuong {chapter_num}: Video raw ton tai nhung sai duration ({video_duration:.2f}s != {duration:.2f}s), re-render...")
                        
                cleanup.register(chapter_raw)
                print(f"  -> Chuong {chapter_num}: Render video ({duration:.2f}s)...")
                if not self.video_renderer.render(duration, chapter_raw):
                    raise RuntimeError(f"Render video that bai cho {audio_file.name}")
                
                chapter_raw_videos.append(chapter_raw)

            merged_raw_video = self.config.temp_dir / f"merged_raw_video_{group.index}.mp4"
            cleanup.register(merged_raw_video)
            
            print(f"[Video {group.index}] Buoc 2b: Concat {len(chapter_raw_videos)} video raw...")
            if not self.audio_mixer.concat_videos(
                video_list=chapter_raw_videos,
                output_path=merged_raw_video,
                temp_dir=self.config.temp_dir
            ):
                raise RuntimeError("Concat raw video that bai")

            # ── Buoc 3: Mix Audio & Nhac Nen ──────────────────────────────
            mixed_video = self.config.temp_dir / f"mixed_video_{group.index}.mp4"
            cleanup.register(mixed_video)
            
            print(f"[Video {group.index}] Buoc 3: Mix video raw voi audio tong + nhac nen...")
            if not self.audio_mixer.mix_audio(
                main_audio=merged_audio,
                video_input=merged_raw_video,
                output_path=mixed_video,
                background_music=self.config.background_music,
                bg_volume=self.config.bg_music_volume,
                audio_duration=total_audio_duration,
                bg_music_loop=self.config.bg_music_loop,
            ):
                raise RuntimeError("Mix audio tong that bai")

            # ── Buoc 4: Noi Intro ─────────────────────────────────────────
            final_video = self.config.output_dir / f"final_video_{group.index}.mp4"
            if not self.config.keep_output:
                cleanup.register(final_video)
                
            if self.config.info_video and self.config.info_video.exists():
                print(f"[Video {group.index}] Buoc 4: Noi intro vao dau video...")
                if not self.audio_mixer.prepend_intro(
                    intro_path=self.config.info_video,
                    main_video=mixed_video,
                    output_path=final_video,
                    temp_dir=self.config.temp_dir
                ):
                    raise RuntimeError("Noi intro that bai")
            else:
                print(f"[Video {group.index}] Buoc 4: Khong co intro, doi ten file mixed thanh final...")
                # Copy hoac move file de giu nguyen ten final_video
                shutil.copy2(mixed_video, final_video)

            # ── Buoc 5: Tao metadata va Upload ──────────────────────────────
            print(f"[Video {group.index}] Tao metadata...")
            title = template.title_pattern.format(index=group.index)
            description = template.description
            if total_groups > 1:
                description += f"\n\n(Tap {group.index}/{total_groups})"
            print(f"  Title: {title}")

            # ── Buoc 4: Upload YouTube ─────────────────────────────────────
            print(f"[Video {group.index}] Upload YouTube...")
            self.uploader.authenticate()
            video_id = self.uploader.upload_video(
                video_path=str(final_video),
                title=title,
                description=description,
                category_id=template.category_id,
                tags=template.tags,
                privacy_status=template.privacy_status,
                altered_content=True,
                playlist_id=template.playlist_id
            )
            if not video_id:
                raise RuntimeError("Upload YouTube that bai")

            # ── Buoc 5: Don dep ────────────────────────────────────────────
            cleanup.cleanup(success=True)
            print(f"[Video {group.index}] HOAN THANH! Video ID: {video_id}")
            return True

        except Exception as e:
            logger.error(f"[Video {group.index}] LOI: {e}")
            cleanup.cleanup(success=False)
            return False

    def run(self):
        """Chay toan bo pipeline."""
        print("=" * 60)
        print("YOUTUBE AUTO-PIPELINE STARTED")
        print("=" * 60)
        print(f"Chapters: {self.config.chapters_dir}")
        print(f"Chapter output: {self.config.chapter_output_dir}")
        print(f"Start index: {self.config.start_index}")
        print(f"Max duration/video: {self.config.max_duration_per_video/3600:.1f}h")
        print(f"Background music: {self.config.background_music}")
        print(f"BG music loop: {self.config.bg_music_loop}")
        print(f"Keep output: {self.config.keep_output}")

        audio_files = self.analyze_data()
        groups = self.split_into_groups(audio_files)

        if not groups:
            logger.warning("Khong co nhom nao de xu ly")
            return

        success_count = 0
        for group in groups:
            if self.process_group(group, len(groups)):
                success_count += 1

        print("=" * 60)
        print(f"PIPELINE KET THUC: {success_count}/{len(groups)} video thanh cong")
        print("=" * 60)


def build_config_from_args(args=None) -> PipelineConfig:
    """Build PipelineConfig tu argparse. Co the dung de parse CLI hoac override."""
    parser = argparse.ArgumentParser(
        description="YouTube Auto-Pipeline",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Vi du CLI:
  python youtube_auto_pipeline.py \\
    --chapters ./chapters \\
    --assets ./assets \\
    --bg-music background.mp3 \\
    --bg-music-loop \\
    --start-index 1 \\
    --title-pattern "Tap {index} | Truyen Audio" \\
    --description "Line 1\\nLine 2" \\
    --tags audio truyen vietnam \\
    --category-id 22 \\
    --privacy private \\
    --playlist-id PLxxxxx
        """
    )

    parser.add_argument("--chapters", type=Path, required=True,
                        help="Thu muc chua file chapter_*.wav")
    parser.add_argument("--assets", type=Path, default=Path("./assets"),
                        help="Thu muc assets (mac dinh: ./assets)")
    parser.add_argument("--temp", type=Path, default=Path("./temp"),
                        help="Thu muc file tam (mac dinh: ./temp)")
    parser.add_argument("--output", type=Path, default=Path("./output"),
                        help="Thu muc output video cuoi (mac dinh: ./output)")
    parser.add_argument("--chapter-output-dir", type=Path, default=None,
                        help="Thu muc output video tung chuong (mac dinh: temp/chapters)")

    parser.add_argument("--start-index", type=int, default=1,
                        help="So bat dau cho title (mac dinh: 1)")
    parser.add_argument("--max-duration", type=float, default=12*3600,
                        help="Thoi luong toi da moi video (giay, mac dinh: 43200 = 12h)")

    parser.add_argument("--title-pattern", type=str, default="Tap {index} | Audio Book",
                        help="Pattern title, dung {index} de thay so tap")
    parser.add_argument("--description", type=str, default="Audio book series - Tap {index}",
                        help="Mo ta video (ho tro \\n de xuong dong)")
    parser.add_argument("--tags", nargs="+", default=["audio", "book"],
                        help="Tags YouTube, cach nhau bang dau cach")
    parser.add_argument("--category-id", type=str, default="22",
                        help="YouTube category ID (mac dinh: 22 = People & Blogs)")
    parser.add_argument("--privacy", type=str, default="private",
                        choices=["private", "unlisted", "public"],
                        help="Trang thai video (mac dinh: private)")
    parser.add_argument("--playlist-id", type=str, default=None,
                        help="Playlist ID de them video vao (optional)")

    parser.add_argument("--bg-music", type=str, default=None,
                        help="Ten file nhac nen trong thu muc assets (optional)")
    parser.add_argument("--bg-volume", type=float, default=0.15,
                        help="Volume nhac nen 0.0-1.0 (mac dinh: 0.15)")
    parser.add_argument("--bg-music-loop", action="store_true",
                        help="Lap lai nhac nen cho moi chuong, voi fade in/out 5s dau/cuoi")

    parser.add_argument("--game-path", type=str, default=None,
                        help="Duong dan den game executable (optional)")
    parser.add_argument("--game-args", type=str, default=None,
                        help="Doi so cau lenh chay game (optional, dung {game_path}, {duration}, {output_path})")

    parser.add_argument("--keep-output", action="store_true",
                        help="Giu lai video sau khi upload (khong xoa)")
    parser.add_argument("--info-video", type=str, default=None,
                        help="Duong dan den file info.mp4 de noi len dau moi video (optional)")


    parsed = parser.parse_args(args)

    bg_music_path = None
    if parsed.bg_music:
        bg_music_path = parsed.assets / parsed.bg_music
        if not bg_music_path.exists():
            logger.warning(f"Khong tim thay nhac nen: {bg_music_path}")
            bg_music_path = None

    template = TemplateConfig(
        title_pattern=parsed.title_pattern,
        description=parsed.description.replace("\\n", "\n"),
        tags=parsed.tags,
        category_id=parsed.category_id,
        privacy_status=parsed.privacy,
        playlist_id=parsed.playlist_id
    )
    info_video_path = None
    if parsed.info_video:
        info_video_path = Path(parsed.info_video)
        if not info_video_path.exists():
            logger.warning(f"Khong tim thay info video: {info_video_path}")
            info_video_path = None

    return PipelineConfig(
        chapters_dir=parsed.chapters,
        temp_dir=parsed.temp,
        output_dir=parsed.output,
        chapter_output_dir=parsed.chapter_output_dir,
        template=template,
        start_index=parsed.start_index,
        max_duration_per_video=parsed.max_duration,
        background_music=bg_music_path,
        bg_music_volume=parsed.bg_volume,
        bg_music_loop=parsed.bg_music_loop,
        keep_output=parsed.keep_output,
        game_path=parsed.game_path,
        game_args=parsed.game_args,
        info_video=info_video_path,
    )


def main():
    """Entry point khi chay tu command line."""
    config = build_config_from_args()
    pipeline = Pipeline(config)
    pipeline.run()


if __name__ == "__main__":
    main()
