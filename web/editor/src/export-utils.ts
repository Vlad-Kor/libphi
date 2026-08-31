export interface ExportDocument {
  path: string;
  name: string;
  text: string;
}

const NOTE_EXTENSION = /\.(?:md|markdown|txt)$/i;

export function noteTitle(path: string): string {
  const name = path.split(/[\\/]/).at(-1) ?? path;
  return name.replace(NOTE_EXTENSION, "");
}
