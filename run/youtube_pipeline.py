#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
YouTube Auto-Pipeline
=====================
Tự động hóa toàn bộ quy trình: ghép audio/subtitle → render video → mix nhạc nền
→ upload YouTube → dọn dẹp file tạm.

Sử dụng:
  1. CLI: python youtube_pipeline.py --chapters ./chapters --subtitles ./subs ...
  2. Python API:
     from youtube_pipeline import Pipeline, PipelineConfig, TemplateConfig
     config = PipelineConfig(...)
     pipeline = Pipeline(config)
     pipeline.run()
"""

import os
import argparse
import logging
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from typing import List, Optional, Tuple
from dataclasses import dataclass, field
from datetime import timedelta
import wave

# Setup logging
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s"
)
logger = logging.getLogger(__name__)

# Import YouTubeUploader từ module bên ngoài
try:
    from youtube_uploader import YouTubeUploader
except ImportError:
    logger.error(
        "Không tìm thấy module 'youtube_uploader'. "
        "Vui lòng đảm bảo file youtube_uploader.py nằm trong PYTHONPATH hoặc cùng thư mục."
    )
    sys.exit(1)


__all__ = [
    "TemplateConfig",
    "PipelineConfig",
    "ChapterGroup",
    "AudioProcessor",
    "SubtitleProcessor",
    "VideoRenderer",
    "AudioMixer",
    "CleanupManager",
    "Pipeline",
    "build_config_from_args",
]


@dataclass
class TemplateConfig:
    """Cấu hình template cho video upload."""
    title_pattern: str
    description: str
    tags: List[str]
    category_id: str
    privacy_status: str
    playlist_id: Optional[str] = None


@dataclass
class PipelineConfig:
    """Cấu hình toàn bộ pipeline."""
    chapters_dir: Path
    subtitles_dir: Path
    temp_dir: Path = field(default_factory=lambda: Path("./temp"))
    output_dir: Path = field(default_factory=lambda: Path("./output"))
    template: Optional[TemplateConfig] = None
    start_index: int = 1
    max_duration_per_video: float = 12 * 3600
    background_music: Optional[Path] = None
    bg_music_volume: float = 0.15
    keep_output: bool = False
    game_path: Optional[str] = None
    game_args: Optional[str] = None
    subtitle_font: Optional[str] = None
    info_video: Optional[Path] = None


@dataclass
class ChapterGroup:
    """Một nhóm chapter được ghép thành 1 video."""
    index: int
    audio_files: List[Path] = field(default_factory=list)
    subtitle_files: List[Path] = field(default_factory=list)
    total_duration: float = 0.0


class AudioProcessor:
    """Xử lý ghép và phân tích audio."""

    @staticmethod
    def get_wav_duration(file_path: Path) -> float:
        try:
            with wave.open(str(file_path), "rb") as wf:
                frames = wf.getnframes()
                rate = wf.getframerate()
                return frames / float(rate)
        except Exception as e:
            logger.error(f"Không đọc được duration của {file_path}: {e}")
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
            print(f"Đã ghép audio ({len(input_files)} files): {output_file}")
            return True
        except subprocess.CalledProcessError as e:
            logger.error(f"Lỗi ghép audio: {e.stderr}")
            return False
        except Exception as e:
            logger.error(f"Lỗi không xác định khi ghép audio: {e}")
            return False


class SubtitleProcessor:
    """Xử lý ghép và offset subtitle SRT."""

    @staticmethod
    def parse_srt_time(time_str: str) -> float:
        time_str = time_str.strip().replace(",", ".")
        parts = time_str.split(":")
        return float(parts[0]) * 3600 + float(parts[1]) * 60 + float(parts[2])

    @staticmethod
    def format_srt_time(seconds: float) -> str:
        td = timedelta(seconds=seconds)
        total_seconds = int(td.total_seconds())
        hrs = total_seconds // 3600
        mins = (total_seconds % 3600) // 60
        secs = total_seconds % 60
        millis = int((seconds - int(seconds)) * 1000)
        return f"{hrs:02d}:{mins:02d}:{secs:02d},{millis:03d}"

    @classmethod
    def merge_srt_files(cls, input_files: List[Path], output_file: Path) -> bool:
        if not input_files:
            return False
        try:
            global_offset = 0.0
            global_index = 1
            with open(output_file, "w", encoding="utf-8") as out_f:
                for srt_file in input_files:
                    if not srt_file.exists():
                        logger.warning(f"Subtitle không tồn tại: {srt_file}")
                        continue
                    file_duration = 0.0
                    with open(srt_file, "r", encoding="utf-8") as in_f:
                        content = in_f.read()
                    blocks = content.strip().split("\n\n")
                    for block in blocks:
                        lines = block.strip().split("\n")
                        if len(lines) < 3:
                            continue
                        time_line = lines[1]
                        if "-->" not in time_line:
                            continue
                        start_str, end_str = time_line.split("-->")
                        start_time = cls.parse_srt_time(start_str) + global_offset
                        end_time = cls.parse_srt_time(end_str) + global_offset
                        if end_time - global_offset > file_duration:
                            file_duration = end_time - global_offset
                        out_f.write(f"{global_index}\n")
                        out_f.write(f"{cls.format_srt_time(start_time)} --> {cls.format_srt_time(end_time)}\n")
                        out_f.write("\n".join(lines[2:]) + "\n\n")
                        global_index += 1
                    global_offset += file_duration
                    print(f"  + {srt_file.name} (duration: {file_duration:.2f}s)")
            print(f"Đã ghép subtitle thành: {output_file}")
            return True
        except Exception as e:
            logger.error(f"Lỗi ghép subtitle: {e}")
            return False


class VideoRenderer:
    """Render video từ subtitle. Hỗ trợ game hoặc ffmpeg fallback."""

    def __init__(self, game_path: Optional[str] = None, game_args: Optional[str] = None, subtitle_font: Optional[str] = None):
        self.game_path = game_path or "flappy_bird"
        self.game_args = game_args or '"{game_path}" --subtitles "{subtitle_path}" --font "{subtitle_font}" --font-size 32 --fps 24 --flap -7.0 -o "{output_path}"'
        self.subtitle_font = subtitle_font or "/content/BeeLeaveRegular-3zR66.ttf"

    def render(self, subtitle_path: Path, output_path: Path) -> bool:
        try:
            if self._check_game():
                return self._render_with_game(subtitle_path, output_path)
            else:
                return self._render_with_ffmpeg(subtitle_path, output_path)
        except Exception as e:
            logger.error(f"Lỗi render video: {e}")
            return False

    def _check_game(self) -> bool:
        try:
            subprocess.run([self.game_path, "--help"], capture_output=True, check=True)
            return True
        except (subprocess.CalledProcessError, FileNotFoundError):
            print(f"Game '{self.game_path}' không khả dụng, sử dụng ffmpeg fallback")
            return False

    def _render_with_game(self, subtitle_path: Path, output_path: Path) -> bool:
        cmd_str = self.game_args.format(
            game_path=Path(self.game_path).as_posix() if self.game_path else "",
            subtitle_path=subtitle_path.as_posix(),
            subtitle_font=Path(self.subtitle_font).as_posix() if self.subtitle_font else "",
            output_path=output_path.as_posix()
        )
        cmd = shlex.split(cmd_str)
        subprocess.run(cmd, capture_output=True, text=True, check=True)
        print(f"Đã render video bằng game: {output_path}")
        return True

    def _render_with_ffmpeg(self, subtitle_path: Path, output_path: Path) -> bool:
        escaped_sub = str(subtitle_path.absolute()).replace("\\", "/").replace(":", "\\:")
        cmd = [
            "ffmpeg", "-y",
            "-f", "lavfi", "-i", "color=c=black:s=1920x1080:r=30",
            "-vf", f"subtitles={escaped_sub}:force_style='FontSize=28,PrimaryColour=&H00FFFFFF,OutlineColour=&H00000000,Outline=2,Shadow=1,MarginV=50'",
            "-c:v", "libx264", "-preset", "medium", "-crf", "23",
            "-pix_fmt", "yuv420p", "-an", "-shortest",
            str(output_path)
        ]
        subprocess.run(cmd, capture_output=True, text=True, check=True)
        print(f"Đã render video bằng ffmpeg: {output_path}")
        return True


class AudioMixer:
    """Ghép audio chính và nhạc nền vào video."""
    
    @staticmethod
    def prepend_intro(
        intro_path: Path,
        main_video: Path,
        output_path: Path,
        temp_dir: Path,
    ) -> bool:
        """
        Nối intro.mp4 lên đầu main_video bằng phương pháp copy (không re-encode).
        Bước 1: Thêm silent audio vào intro nếu chưa có (để đồng bộ stream với main video).
        Bước 2: Concat bằng -c copy.
        """
        intro_with_audio = temp_dir / f"intro_a_{os.getpid()}.mp4"
        concat_list      = temp_dir / f"concat_list_intro_{os.getpid()}.txt"
        try:
            # Bước 1 – Gắn silent audio vào intro (giống code mẫu anullsrc)
            cmd_intro = [
                "ffmpeg", "-y",
                "-i", str(intro_path),
                "-f", "lavfi", "-i", "anullsrc=r=44100:cl=stereo",
                "-c:v", "copy",
                "-c:a", "aac", "-b:a", "192k",
                "-shortest",
                str(intro_with_audio),
            ]
            subprocess.run(cmd_intro, capture_output=True, text=True, check=True)
            print(f"Đã thêm silent audio vào intro: {intro_with_audio}")

            # Bước 2 – Tạo concat list rồi ghép
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
            print(f"Đã nối intro vào đầu video: {output_path}")
            return True

        except subprocess.CalledProcessError as e:
            logger.error(f"Lỗi nối intro: {e.stderr}")
            return False
        except Exception as e:
            logger.error(f"Lỗi không xác định khi nối intro: {e}")
            return False
        finally:
            intro_with_audio.unlink(missing_ok=True)
            concat_list.unlink(missing_ok=True)
    
    @staticmethod
    def mix_audio(
        main_audio: Path,
        video_input: Path,
        output_path: Path,
        background_music: Optional[Path] = None,
        bg_volume: float = 0.15
    ) -> bool:
        try:
            if background_music and background_music.exists():
                cmd = [
                    "ffmpeg", "-y",
                    "-i", str(video_input), "-i", str(main_audio),
                    "-stream_loop", "-1", "-i", str(background_music),
                    "-filter_complex",
                    f"[2:a]aloop=loop=-1:size=0,volume={bg_volume}[bg];[1:a][bg]amix=inputs=2:duration=first:dropout_transition=3[aout]",
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
            print(f"Đã mix audio: {output_path}")
            return True
        except subprocess.CalledProcessError as e:
            logger.error(f"Lỗi mix audio: {e.stderr}")
            return False
        except Exception as e:
            logger.error(f"Lỗi không xác định khi mix audio: {e}")
            return False


class CleanupManager:
    """Quản lý dọn dẹp file tạm. Chỉ xóa sau khi upload thành công."""

    def __init__(self, keep_output: bool = False):
        self.keep_output = keep_output
        self.temp_files: List[Path] = []

    def register(self, file_path: Path):
        self.temp_files.append(file_path)

    def cleanup(self, success: bool = True):
        if not success:
            print("Upload thất bại, giữ lại file để debug/retry")
            return
        for file_path in self.temp_files:
            try:
                if file_path.exists():
                    if file_path.is_file():
                        file_path.unlink()
                        print(f"Đã xóa file: {file_path}")
                    elif file_path.is_dir():
                        shutil.rmtree(file_path)
                        print(f"Đã xóa thư mục: {file_path}")
            except Exception as e:
                logger.warning(f"Không thể xóa {file_path}: {e}")
        self.temp_files.clear()


class Pipeline:
    """Pipeline chính điều phối toàn bộ quy trình."""

    def __init__(self, config: PipelineConfig):
        self.config = config
        self.audio_processor = AudioProcessor()
        self.subtitle_processor = SubtitleProcessor()
        self.video_renderer = VideoRenderer(
            game_path=config.game_path,
            game_args=config.game_args,
            subtitle_font=config.subtitle_font
        )
        self.audio_mixer = AudioMixer()
        self.uploader = YouTubeUploader()

        self.config.temp_dir.mkdir(parents=True, exist_ok=True)
        self.config.output_dir.mkdir(parents=True, exist_ok=True)

    def analyze_data(self) -> Tuple[List[Path], List[Path]]:
        audio_files = sorted(self.config.chapters_dir.glob("chapter_*.wav"))
        subtitle_files = sorted(self.config.subtitles_dir.glob("chapter_*.srt"))

        if not audio_files:
            raise ValueError(f"Không tìm thấy file audio trong {self.config.chapters_dir}")

        valid_pairs = []
        for audio in audio_files:
            expected_sub = self.config.subtitles_dir / audio.with_suffix(".srt").name
            if expected_sub.exists():
                valid_pairs.append((audio, expected_sub))
            else:
                logger.warning(f"Không tìm thấy subtitle cho {audio.name}")

        if not valid_pairs:
            raise ValueError("Không có cặp audio-subtitle hợp lệ nào")

        print(f"Tìm thấy {len(valid_pairs)} cặp audio-subtitle")
        return [p[0] for p in valid_pairs], [p[1] for p in valid_pairs]

    def split_into_groups(self, audio_files: List[Path], subtitle_files: List[Path]) -> List[ChapterGroup]:
        # Tính duration tất cả chapter
        durations = [self.audio_processor.get_wav_duration(f) for f in audio_files]
        total_duration = sum(durations)

        # Tính số group cần thiết (làm tròn lên)
        import math
        num_groups = max(1, math.ceil(total_duration / self.config.max_duration_per_video))
        target_per_group = total_duration / num_groups

        print(f"Tổng duration: {total_duration/3600:.2f}h → chia thành {num_groups} group, mỗi group ~{target_per_group/3600:.2f}h")

        groups = []
        current_group = ChapterGroup(index=self.config.start_index)

        for audio, subtitle, duration in zip(audio_files, subtitle_files, durations):
            # Nếu group hiện tại đã đủ target VÀ còn chapter phía sau → flush
            if (current_group.total_duration + duration > target_per_group
                    and current_group.audio_files
                    and len(groups) < num_groups - 1):   # giữ lại slot cho group cuối
                groups.append(current_group)
                current_group = ChapterGroup(index=current_group.index + 1)

            current_group.audio_files.append(audio)
            current_group.subtitle_files.append(subtitle)
            current_group.total_duration += duration

        if current_group.audio_files:
            groups.append(current_group)

        print(f"Đã chia thành {len(groups)} nhóm:")
        for g in groups:
            print(f"  Video {g.index}: {len(g.audio_files)} chapters, {g.total_duration/3600:.2f}h")
        return groups
    

    def process_group(self, group: ChapterGroup, total_groups: int) -> bool:
        cleanup = CleanupManager(keep_output=self.config.keep_output)
        template = self.config.template
        if template is None:
            raise ValueError("PipelineConfig.template không được để None")

        try:
            merged_audio = self.config.temp_dir / f"merged_audio_{group.index}.wav"
            merged_subtitle = self.config.temp_dir / f"merged_subtitle_{group.index}.srt"
            raw_video = self.config.temp_dir / f"raw_video_{group.index}.mp4"
            final_video = self.config.output_dir / f"final_video_{group.index}.mp4"

            cleanup.register(merged_audio)
            cleanup.register(merged_subtitle)
            cleanup.register(raw_video)
            if not self.config.keep_output:
                cleanup.register(final_video)

            # 4.1 Ghép audio
            print(f"[Video {group.index}] Bước 1/6: Ghép audio ({len(group.audio_files)} files)...")
            if not self.audio_processor.merge_wav_files(group.audio_files, merged_audio):
                raise RuntimeError("Ghép audio thất bại")

            # 4.2 Ghép subtitle
            print(f"[Video {group.index}] Bước 2/6: Ghép subtitle...")
            if not self.subtitle_processor.merge_srt_files(group.subtitle_files, merged_subtitle):
                raise RuntimeError("Ghép subtitle thất bại")

            # 4.3 Tạo video
            print(f"[Video {group.index}] Bước 3/6: Render video...")
            if not self.video_renderer.render(merged_subtitle, raw_video):
                raise RuntimeError("Render video thất bại")

            # 4.4 Ghép âm thanh + nhạc nền
            print(f"[Video {group.index}] Bước 4/6: Mix audio + nhạc nền...")
            if not self.audio_mixer.mix_audio(
                merged_audio, raw_video, final_video,
                self.config.background_music, self.config.bg_music_volume
            ):
                raise RuntimeError("Mix audio thất bại")
            # ── THÊM BƯỚC MỚI ─────────────────────────────────────────────
            # 4.5 Nối info.mp4 lên đầu video (copy, không re-encode)
            
            if self.config.info_video and self.config.info_video.exists():
                print(f"[Video {group.index}] Bước 5/7: Nối info.mp4 lên đầu video...")
                video_with_intro = self.config.output_dir / f"final_video_{group.index}_intro.mp4"
                cleanup.register(final_video)            # final_video cũ trở thành temp
                if not self.config.keep_output:
                    cleanup.register(video_with_intro)
                if not self.audio_mixer.prepend_intro(
                    self.config.info_video, final_video,
                    video_with_intro, self.config.temp_dir
                ):
                    raise RuntimeError("Nối intro thất bại")
                final_video = video_with_intro           # upload file đã có intro
            else:
                print(f"[Video {group.index}] Bước 5/7: Bỏ qua (không có info_video)")
            # ──────────────────────────────────────────────────────────────
            # 4.5 Tạo metadata
            print(f"[Video {group.index}] Bước 5/6: Tạo metadata...")
            title = template.title_pattern.format(index=group.index)
            description = template.description
            if total_groups > 1:
                description += f"\n\n(Tập {group.index}/{total_groups})"
            print(f"  Title: {title}")

            # 4.6 Upload YouTube
            print(f"[Video {group.index}] Bước 6/6: Upload YouTube...")
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
                raise RuntimeError("Upload YouTube thất bại")

            # 5. Dọn dẹp
            cleanup.cleanup(success=True)
            print(f"[Video {group.index}] HOÀN THÀNH! Video ID: {video_id}")
            return True

        except Exception as e:
            logger.error(f"[Video {group.index}] LỖI: {e}")
            cleanup.cleanup(success=False)
            return False

    def run(self):
        """Chạy toàn bộ pipeline."""
        print("=" * 60)
        print("YOUTUBE AUTO-PIPELINE STARTED")
        print("=" * 60)
        print(f"Chapters: {self.config.chapters_dir}")
        print(f"Subtitles: {self.config.subtitles_dir}")
        print(f"Start index: {self.config.start_index}")
        print(f"Max duration/video: {self.config.max_duration_per_video/3600:.1f}h")
        print(f"Background music: {self.config.background_music}")
        print(f"Keep output: {self.config.keep_output}")

        audio_files, subtitle_files = self.analyze_data()
        groups = self.split_into_groups(audio_files, subtitle_files)

        if not groups:
            logger.warning("Không có nhóm nào để xử lý")
            return

        success_count = 0
        for group in groups:
            if self.process_group(group, len(groups)):
                success_count += 1

        print("=" * 60)
        print(f"PIPELINE KẾT THÚC: {success_count}/{len(groups)} video thành công")
        print("=" * 60)


def build_config_from_args(args=None) -> PipelineConfig:
    """Build PipelineConfig từ argparse. Có thể dùng để parse CLI hoặc override."""
    parser = argparse.ArgumentParser(
        description="YouTube Auto-Pipeline",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Ví dụ CLI:
  python youtube_pipeline.py \
    --chapters ./chapters \
    --subtitles ./chapters/subtitles \
    --assets ./assets \
    --bg-music background.mp3 \
    --start-index 1 \
    --title-pattern "Tập {index} | Truyện Audio" \
    --description "Line 1\nLine 2" \
    --tags audio truyen vietnam \
    --category-id 22 \
    --privacy private \
    --playlist-id PLxxxxx
        """
    )

    parser.add_argument("--chapters", type=Path, required=True,
                        help="Thư mục chứa file chapter_*.wav")
    parser.add_argument("--subtitles", type=Path, required=True,
                        help="Thư mục chứa file chapter_*.srt")
    parser.add_argument("--assets", type=Path, default=Path("./assets"),
                        help="Thư mục assets (mặc định: ./assets)")
    parser.add_argument("--temp", type=Path, default=Path("./temp"),
                        help="Thư mục file tạm (mặc định: ./temp)")
    parser.add_argument("--output", type=Path, default=Path("./output"),
                        help="Thư mục output video (mặc định: ./output)")

    parser.add_argument("--start-index", type=int, default=1,
                        help="Số bắt đầu cho title (mặc định: 1)")
    parser.add_argument("--max-duration", type=float, default=12*3600,
                        help="Thởi lượng tối đa mỗi video (giây, mặc định: 43200 = 12h)")

    parser.add_argument("--title-pattern", type=str, default="Tập {index} | Audio Book",
                        help="Pattern title, dùng {index} để thay số tập")
    parser.add_argument("--description", type=str, default="Audio book series - Tập {index}",
                        help="Mô tả video (hỗ trợ \n để xuống dòng)")
    parser.add_argument("--tags", nargs="+", default=["audio", "book"],
                        help="Tags YouTube, cách nhau bằng dấu cách")
    parser.add_argument("--category-id", type=str, default="22",
                        help="YouTube category ID (mặc định: 22 = People & Blogs)")
    parser.add_argument("--privacy", type=str, default="private",
                        choices=["private", "unlisted", "public"],
                        help="Trạng thái video (mặc định: private)")
    parser.add_argument("--playlist-id", type=str, default=None,
                        help="Playlist ID để thêm video vào (optional)")

    parser.add_argument("--bg-music", type=str, default=None,
                        help="Tên file nhạc nền trong thư mục assets (optional)")
    parser.add_argument("--bg-volume", type=float, default=0.15,
                        help="Volume nhạc nền 0.0-1.0 (mặc định: 0.15)")

    parser.add_argument("--game-path", type=str, default=None,
                        help="Đường dẫn đến game executable (optional)")
    parser.add_argument("--game-args", type=str, default=None,
                        help="Đối số câu lệnh chạy game (optional)")

    parser.add_argument("--keep-output", action="store_true",
                        help="Giữ lại video sau khi upload (không xóa)")
    parser.add_argument("--subtitle_font", type=str, default=None,
                        help="Font cho subtitle")				
    parser.add_argument("--info-video", type=str, default=None,
                        help="Đường dẫn đến file info.mp4 để nối lên đầu mỗi video (optional)")                        
						

    parsed = parser.parse_args(args)

    bg_music_path = None
    if parsed.bg_music:
        bg_music_path = parsed.assets / parsed.bg_music
        if not bg_music_path.exists():
            logger.warning(f"Không tìm thấy nhạc nền: {bg_music_path}")
            bg_music_path = None

    template = TemplateConfig(
        title_pattern=parsed.title_pattern,
        description=parsed.description.replace("\n", "\n"),
        tags=parsed.tags,
        category_id=parsed.category_id,
        privacy_status=parsed.privacy,
        playlist_id=parsed.playlist_id
    )
    info_video_path = None
    if parsed.info_video:
        info_video_path = Path(parsed.info_video)
        if not info_video_path.exists():
            logger.warning(f"Không tìm thấy info video: {info_video_path}")
            info_video_path = None


    return PipelineConfig(
        chapters_dir=parsed.chapters,
        subtitles_dir=parsed.subtitles,
        temp_dir=parsed.temp,
        output_dir=parsed.output,
        template=template,
        start_index=parsed.start_index,
        max_duration_per_video=parsed.max_duration,
        background_music=bg_music_path,
        bg_music_volume=parsed.bg_volume,
        keep_output=parsed.keep_output,
        game_path=parsed.game_path,
        game_args=parsed.game_args,
		subtitle_font = parsed.subtitle_font,
        info_video=info_video_path,
    )


def main():
    """Entry point khi chạy từ command line."""
    config = build_config_from_args()
    pipeline = Pipeline(config)
    pipeline.run()


if __name__ == "__main__":
    main()

