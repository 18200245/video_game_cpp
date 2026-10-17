#!/usr/bin/env python3
"""
YouTube Upload Module - Hỗ trợ SSH (sửa lỗi redirect_uri)
Tách biệt login và upload, đầy đủ metadata.
"""

import os
import sys
import json
import argparse
from typing import List, Optional

from google.auth.transport.requests import Request
from google.oauth2.credentials import Credentials
from google_auth_oauthlib.flow import InstalledAppFlow
from googleapiclient.discovery import build
from googleapiclient.http import MediaFileUpload
from googleapiclient.errors import HttpError

SCOPES = [
    "https://www.googleapis.com/auth/youtube.upload",
    "https://www.googleapis.com/auth/youtube.force-ssl"
]
DEFAULT_CATEGORY = "22"


class YouTubeUploader:
    def __init__(self, client_secrets_file: str = "client_secrets.json", credentials_file: str = "youtube_token.json"):
        self.client_secrets_file = client_secrets_file
        self.credentials_file = credentials_file
        self.service = None

    def authenticate(self, force_new: bool = False) -> bool:
        creds = None

        if not force_new and os.path.exists(self.credentials_file):
            try:
                creds = Credentials.from_authorized_user_file(self.credentials_file, SCOPES)
                print("Đã tìm thấy token cũ.")
            except Exception as e:
                print(f"Lỗi đọc token: {e}", file=sys.stderr)

        if creds and creds.valid:
            print("Token còn hiệu lực.")
            self.service = build("youtube", "v3", credentials=creds)
            return True

        if creds and creds.expired and creds.refresh_token:
            try:
                creds.refresh(Request())
                print("Đã làm mới token.")
                self.service = build("youtube", "v3", credentials=creds)
                self._save_credentials(creds)
                return True
            except Exception as e:
                print(f"Làm mới token thất bại: {e}", file=sys.stderr)

        # Đăng nhập mới bằng OAuth thủ công (nhập code)
        if not os.path.exists(self.client_secrets_file):
            raise FileNotFoundError(f"Không thấy file {self.client_secrets_file}")

        flow = InstalledAppFlow.from_client_secrets_file(self.client_secrets_file, SCOPES)
        flow.redirect_uri = 'urn:ietf:wg:oauth:2.0:oob'  # Quan trọng: dùng cho headless

        auth_url, _ = flow.authorization_url(prompt='consent', access_type='offline')

        print("\n" + "="*70)
        print("🔐 XÁC THỰC TÀI KHOẢN YOUTUBE (NHẬP CODE)")
        print("="*70)
        print("1. Mở trình duyệt trên máy tính CỦA BẠN và truy cập link sau:")
        print(f"\n   {auth_url}\n")
        print("2. Đăng nhập tài khoản Google, chấp nhận quyền.")
        print("3. Sau khi cấp quyền, bạn sẽ thấy một mã code (dạng 4/xxx...).")
        print("4. Sao chép mã code đó và dán vào đây.")
        print("="*70)

        code = input("Paste the authorization code: ").strip()
        try:
            flow.fetch_token(code=code)
        except Exception as e:
            print(f"\n❌ Lỗi khi trao đổi code: {e}")
            return False

        creds = flow.credentials
        self.service = build("youtube", "v3", credentials=creds)
        self._save_credentials(creds)
        print("✅ Đăng nhập thành công!")
        return True

    def _save_credentials(self, creds):
        with open(self.credentials_file, "w") as f:
            f.write(creds.to_json())
        print(f"📁 Token lưu tại: {self.credentials_file}")

    def load_existing_token(self) -> bool:
        if not os.path.exists(self.credentials_file):
            print("❌ Chưa có token. Hãy chạy --auth trước.", file=sys.stderr)
            return False
        try:
            creds = Credentials.from_authorized_user_file(self.credentials_file, SCOPES)
            if not creds or not creds.valid:
                print("❌ Token không hợp lệ hoặc hết hạn. Chạy lại --auth.", file=sys.stderr)
                return False
            self.service = build("youtube", "v3", credentials=creds)
            return True
        except Exception as e:
            print(f"Lỗi load token: {e}", file=sys.stderr)
            return False

    def upload_video(self, video_path: str, title: str, description: str = "",
                     category_id: str = DEFAULT_CATEGORY, tags: Optional[List[str]] = None,
                     privacy_status: str = "public", altered_content: bool = False,
                     playlist_id: Optional[str] = None) -> str:
        if not self.service:
            raise RuntimeError("Chưa xác thực. Gọi authenticate() hoặc load_existing_token() trước.")

        if not os.path.exists(video_path):
            raise FileNotFoundError(f"Không tìm thấy file: {video_path}")

        body = {
            "snippet": {
                "title": title[:100],
                "description": description[:5000],
                "tags": tags[:500] if tags else [],
                "categoryId": category_id
            },
            "status": {
                "privacyStatus": privacy_status,
                "selfDeclaredMadeForKids": False,
                "containsSyntheticMedia": altered_content
            }
        }

        media = MediaFileUpload(video_path, chunksize=-1, resumable=True)
        print(f"🚀 Đang upload: {title}")
        try:
            request = self.service.videos().insert(part="snippet,status", body=body, media_body=media)
            response = request.execute()
            video_id = response["id"]
            print(f"✅ Upload thành công! Video ID: {video_id}")

            if playlist_id:
                self._add_to_playlist(video_id, playlist_id)
            return video_id
        except HttpError as e:
            error_detail = json.loads(e.content.decode()) if e.content else {}
            msg = error_detail.get("error", {}).get("message", str(e))
            raise RuntimeError(f"YouTube API lỗi: {msg}")

    def _add_to_playlist(self, video_id: str, playlist_id: str):
        try:
            self.service.playlistItems().insert(
                part="snippet",
                body={
                    "snippet": {
                        "playlistId": playlist_id,
                        "resourceId": {"kind": "youtube#video", "videoId": video_id}
                    }
                }
            ).execute()
            print(f"📋 Đã thêm video vào playlist {playlist_id}")
        except HttpError as e:
            print(f"⚠️ Không thêm được playlist: {e}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="YouTube Uploader - SSH friendly")
    parser.add_argument("--auth", action="store_true", help="Chỉ đăng nhập, lưu token")
    parser.add_argument("--upload", action="store_true", help="Upload video dùng token có sẵn")
    parser.add_argument("--file", help="Đường dẫn video")
    parser.add_argument("--title", help="Tiêu đề (mặc định: tên file)")
    parser.add_argument("--description", default="", help="Mô tả")
    parser.add_argument("--category", default=DEFAULT_CATEGORY, help=f"Category ID (mặc định: {DEFAULT_CATEGORY})")
    parser.add_argument("--tags", help="Tag cách nhau bằng dấu phẩy")
    parser.add_argument("--privacy", choices=["public", "unlisted", "private"], default="public")
    parser.add_argument("--altered-content", action="store_true", help="Đánh dấu nội dung AI/synthetic")
    parser.add_argument("--playlist", help="ID playlist")
    parser.add_argument("--client-secrets", default="client_secrets.json")
    parser.add_argument("--credentials", default="youtube_token.json")

    args = parser.parse_args()
    uploader = YouTubeUploader(args.client_secrets, args.credentials)

    if args.auth:
        success = uploader.authenticate(force_new=True)
        sys.exit(0 if success else 1)

    if args.upload:
        if not args.file:
            print("❌ Lỗi: --upload cần --file", file=sys.stderr)
            sys.exit(1)

        if not uploader.load_existing_token():
            print("💡 Hãy chạy --auth trước để tạo token.", file=sys.stderr)
            sys.exit(1)

        title = args.title if args.title else os.path.splitext(os.path.basename(args.file))[0]
        tags_list = [t.strip() for t in args.tags.split(",")] if args.tags else None

        try:
            video_id = uploader.upload_video(
                video_path=args.file,
                title=title,
                description=args.description,
                category_id=args.category,
                tags=tags_list,
                privacy_status=args.privacy,
                altered_content=args.altered_content,
                playlist_id=args.playlist
            )
            print(f"\n🎉 Xem tại: https://www.youtube.com/watch?v={video_id}")
        except Exception as e:
            print(f"\n❌ Upload thất bại: {e}", file=sys.stderr)
            sys.exit(1)
    else:
        parser.print_help()
        print("\n👉 Dùng --auth để đăng nhập, --upload để tải video.")


if __name__ == "__main__":
    main()