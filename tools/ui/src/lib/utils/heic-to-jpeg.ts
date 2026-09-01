import { IMAGE } from '$lib/constants';
import { MimeTypeImage } from '$lib/enums';
import heicToModuleUrl from 'heic-to/csp?url';

// HEIC needs a relatively large decoder. Vite emits it as a local, hashed asset;
// the runtime import remains lazy without executing third-party CDN code in the
// page that holds private attachments.

type HeicToModule = typeof import('heic-to/csp');

let modulePromise: Promise<HeicToModule> | null = null;

/**
 * Lazily load the bundled local heic-to decoder and cache it
 * @returns Promise resolving to the heic-to module
 */
function getHeicTo(): Promise<HeicToModule> {
	if (!modulePromise) {
		modulePromise = import(/* @vite-ignore */ heicToModuleUrl) as Promise<HeicToModule>;
	}

	return modulePromise;
}

/**
 * Convert a HEIC/HEIF file to a compressed JPEG data URL
 * @param file - The HEIC/HEIF file to convert
 * @returns Promise resolving to JPEG data URL
 */
export async function heicFileToJpegDataURL(file: File | Blob): Promise<string> {
	const { heicTo } = await getHeicTo();
	const jpegBlob = await heicTo({
		blob: file,
		quality: IMAGE.HEIC_JPEG_QUALITY,
		type: MimeTypeImage.JPEG
	});

	return new Promise((resolve, reject) => {
		const reader = new FileReader();

		reader.onload = () => resolve(reader.result as string);
		reader.onerror = () => reject(reader.error);
		reader.readAsDataURL(jpegBlob);
	});
}

/**
 * Check if a MIME type represents a HEIC/HEIF image
 * @param mimeType - The MIME type to check
 * @returns True if the MIME type is image/heic or image/heif
 */
export function isHeicMimeType(mimeType: string): boolean {
	const normalized = mimeType.trim().toLowerCase();

	return normalized === MimeTypeImage.HEIC || normalized === MimeTypeImage.HEIF;
}
